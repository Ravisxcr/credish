/*
 * RDB persistence — binary snapshot format.
 *
 * File layout:
 *   CREDISH_RDB\n          — 10-byte magic
 *   uint8  version         — format version (currently 1)
 *   [ for each non-empty db:
 *       uint8  SECTION_DB
 *       uint32 db_id
 *       uint32 key_count
 *       [ for each key:
 *           uint8  type        (OBJ_STRING | OBJ_LIST | ...)
 *           uint32 key_len
 *           char[] key
 *           uint8  has_expire
 *           int64  expire_ms   (only if has_expire)
 *           <type-specific value encoding>
 *       ]
 *   ]
 *   uint8  SECTION_EOF
 *   uint32 crc32            — checksum of everything above
 */

#include "rdb.h"
#include "../db.h"
#include "../sds.h"
#include "../object.h"
#include "../adlist.h"
#include "../skiplist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "platform.h"

#define RDB_MAGIC    "CREDISH_RDB\n"
#define RDB_VERSION  2
#define SECTION_DB   0xFE
#define SECTION_EOF  0xFF

static char rdb_path(char *buf, size_t len, const char *data_dir) {
    snprintf(buf, len, "%s/credish.rdb", data_dir);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Simple CRC32 (table-based)                                          */
/* ------------------------------------------------------------------ */

static uint32_t crc32_table[256];
static int       crc32_init_done = 0;

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_init_done = 1;
}

static uint32_t crc32_update(uint32_t crc, const void *buf, size_t len) {
    if (!crc32_init_done) crc32_init();
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ------------------------------------------------------------------ */
/* Write helpers                                                        */
/* ------------------------------------------------------------------ */

static uint32_t g_crc;

static int rdb_tmp_path(char *buf, size_t len, const char *data_dir) {
    char path[600];
    rdb_path(path, sizeof(path), data_dir);
    snprintf(buf, len, "%s.tmp", path);
    return 0;
}

static int w_raw(FILE *f, const void *data, size_t len) {
    return fwrite(data, 1, len, f) == len ? 0 : -1;
}

static int w_u8(FILE *f, uint8_t v) {
    if (w_raw(f, &v, 1) != 0) return -1;
    g_crc = crc32_update(g_crc, &v, 1);
    return 0;
}

static int w_u32(FILE *f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8),  (uint8_t)v };
    if (w_raw(f, b, 4) != 0) return -1;
    g_crc = crc32_update(g_crc, b, 4);
    return 0;
}

static int w_i64(FILE *f, int64_t v) {
    uint64_t u = (uint64_t)v;
    uint8_t b[8];
    for (int i = 7; i >= 0; i--) { b[i] = u & 0xFF; u >>= 8; }
    if (w_raw(f, b, 8) != 0) return -1;
    g_crc = crc32_update(g_crc, b, 8);
    return 0;
}

static int w_blob(FILE *f, const char *data, uint32_t len) {
    if (w_u32(f, len) != 0) return -1;
    if (w_raw(f, data, len) != 0) return -1;
    g_crc = crc32_update(g_crc, data, len);
    return 0;
}

static int w_sds(FILE *f, sds s) {
    return w_blob(f, s, (uint32_t)SDS_LEN(s));
}

/* ------------------------------------------------------------------ */
/* Value serialisation                                                  */
/* ------------------------------------------------------------------ */

static int save_string(FILE *f, credishObject *o) {
    sds s = (sds)o->ptr;
    return w_sds(f, s);
}

static int save_list(FILE *f, credishObject *o) {
    adlist *l = (adlist *)o->ptr;
    if (w_u32(f, (uint32_t)l->len) != 0) return -1;
    adlist_node *n = l->head;
    while (n) {
        if (w_sds(f, (sds)n->value) != 0) return -1;
        n = n->next;
    }
    return 0;
}

static int save_hash(FILE *f, credishObject *o) {
    dict *d = (dict *)o->ptr;
    if (w_u32(f, (uint32_t)dict_size(d)) != 0) return -1;
    dictIterator *it = dict_iter_new(d);
    if (!it) return -1;
    dictEntry *e;
    while ((e = dict_iter_next(it))) {
        if (w_sds(f, (sds)e->key) != 0 || w_sds(f, (sds)e->v.val) != 0) {
            dict_iter_free(it);
            return -1;
        }
    }
    dict_iter_free(it);
    return 0;
}

static int save_set(FILE *f, credishObject *o) {
    dict *d = (dict *)o->ptr;
    if (w_u32(f, (uint32_t)dict_size(d)) != 0) return -1;
    dictIterator *it = dict_iter_new(d);
    if (!it) return -1;
    dictEntry *e;
    while ((e = dict_iter_next(it)))
        if (w_sds(f, (sds)e->key) != 0) {
            dict_iter_free(it);
            return -1;
        }
    dict_iter_free(it);
    return 0;
}

static int save_zset(FILE *f, credishObject *o) {
    zset *zs = (zset *)o->ptr;
    if (w_u32(f, (uint32_t)dict_size(zs->dict)) != 0) return -1;
    dictIterator *it = dict_iter_new(zs->dict);
    if (!it) return -1;
    dictEntry *e;
    while ((e = dict_iter_next(it))) {
        if (w_sds(f, (sds)e->key) != 0) {
            dict_iter_free(it);
            return -1;
        }
        double score = *(double *)e->v.val;
        uint64_t raw;
        memcpy(&raw, &score, 8);
        uint8_t b[8];
        for (int i = 7; i >= 0; i--) { b[i] = raw & 0xFF; raw >>= 8; }
        if (w_raw(f, b, 8) != 0) {
            dict_iter_free(it);
            return -1;
        }
        g_crc = crc32_update(g_crc, b, 8);
    }
    dict_iter_free(it);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Save                                                                 */
/* ------------------------------------------------------------------ */

int rdb_save(credish_store *store) {
    char path[600];
    rdb_path(path, sizeof(path), store->config.data_dir);
    char tmp[608];
    rdb_tmp_path(tmp, sizeof(tmp), store->config.data_dir);
    remove(tmp);

    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;

    g_crc = 0;
    /* Magic + version */
    size_t mlen = strlen(RDB_MAGIC);
    if (w_raw(f, RDB_MAGIC, mlen) != 0) goto fail;
    g_crc = crc32_update(g_crc, RDB_MAGIC, mlen);
    if (w_u8(f, RDB_VERSION) != 0) goto fail;

    for (int i = 0; i < CREDISH_DB_COUNT; i++) {
        credish_db *db = &store->dbs[i];
        if (dict_size(db->keys) == 0) continue;

        if (w_u8(f, SECTION_DB) != 0 ||
            w_u32(f, (uint32_t)i) != 0 ||
            w_u32(f, (uint32_t)dict_size(db->keys)) != 0)
            goto fail;

        dictIterator *it = dict_iter_new(db->keys);
        if (!it) goto fail;
        dictEntry    *e;
        while ((e = dict_iter_next(it))) {
            credishObject *o = (credishObject *)e->v.val;
            if (w_u8(f, (uint8_t)o->type) != 0) { dict_iter_free(it); goto fail; }

            sds key = (sds)e->key;
            if (w_sds(f, key) != 0) { dict_iter_free(it); goto fail; }

            /* Expiry */
            int64_t dl = db_get_expire(db, key, (int)SDS_LEN(key));
            if (dl >= 0) {
                if (w_u8(f, 1) != 0 || w_i64(f, dl) != 0) { dict_iter_free(it); goto fail; }
            } else if (w_u8(f, 0) != 0) { dict_iter_free(it); goto fail; }

            if (w_u8(f, (uint8_t)o->encoding) != 0) { dict_iter_free(it); goto fail; }

            switch (o->type) {
            case OBJ_STRING: if (save_string(f, o) != 0) { dict_iter_free(it); goto fail; } break;
            case OBJ_LIST:   if (save_list(f, o) != 0)   { dict_iter_free(it); goto fail; } break;
            case OBJ_HASH:   if (save_hash(f, o) != 0)   { dict_iter_free(it); goto fail; } break;
            case OBJ_SET:    if (save_set(f, o) != 0)    { dict_iter_free(it); goto fail; } break;
            case OBJ_ZSET:   if (save_zset(f, o) != 0)   { dict_iter_free(it); goto fail; } break;
            default: dict_iter_free(it); goto fail;
            }
        }
        dict_iter_free(it);
    }

    if (w_u8(f, SECTION_EOF) != 0) goto fail;
    uint32_t final_crc = g_crc;
    uint8_t cb[4] = { (uint8_t)(final_crc >> 24), (uint8_t)(final_crc >> 16),
                      (uint8_t)(final_crc >> 8),  (uint8_t)final_crc };
    if (w_raw(f, cb, 4) != 0) goto fail;
    if (credish_fsync_file(f) != 0) goto fail;
    if (fclose(f) != 0) { remove(tmp); return -1; }
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    credish_fsync_parent_dir(path);
    store->last_save_time = (int64_t)time(NULL);
    return 0;

fail:
    fclose(f);
    remove(tmp);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Load                                                                 */
/* ------------------------------------------------------------------ */

static uint8_t  r_u8(FILE *f)  { uint8_t v = 0; fread(&v, 1, 1, f); return v; }
static uint32_t r_u32(FILE *f) {
    uint8_t b[4]; fread(b, 1, 4, f);
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}
static int64_t r_i64(FILE *f) {
    uint8_t b[8]; fread(b, 1, 8, f);
    int64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
    return v;
}
static sds r_sds(FILE *f) {
    uint32_t len = r_u32(f);
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    fread(buf, 1, len, f);
    buf[len] = '\0';
    sds s = sds_newlen(buf, len);
    free(buf);
    return s;
}

int rdb_load(credish_store *store) {
    char path[600];
    rdb_path(path, sizeof(path), store->config.data_dir);
    char tmp[608];
    rdb_tmp_path(tmp, sizeof(tmp), store->config.data_dir);
    remove(tmp);

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char magic[12] = {0};
    size_t mlen = strlen(RDB_MAGIC);
    if (fread(magic, 1, mlen, f) != mlen || memcmp(magic, RDB_MAGIC, mlen) != 0) {
        fclose(f); return -1;
    }
    uint8_t ver = r_u8(f);
    if (ver != 1 && ver != RDB_VERSION) { fclose(f); return -1; }

    while (1) {
        uint8_t section = r_u8(f);
        if (section == SECTION_EOF) break;
        if (section != SECTION_DB) { fclose(f); return -1; }

        int db_id = (int)r_u32(f);
        if (db_id < 0 || db_id >= CREDISH_DB_COUNT) { fclose(f); return -1; }
        credish_db *db = &store->dbs[db_id];
        uint32_t count = r_u32(f);

        for (uint32_t i = 0; i < count; i++) {
            int type = (int)r_u8(f);
            sds key  = r_sds(f);

            uint8_t has_exp = r_u8(f);
            int64_t dl = has_exp ? r_i64(f) : -1;
            int encoding = ver >= 2 ? (int)r_u8(f) : OBJ_ENCODING_RAW;

            credishObject *o = NULL;
            switch (type) {
            case OBJ_STRING: {
                sds val = r_sds(f);
                o = obj_create_string_encoded(val, (int)SDS_LEN(val), encoding);
                sds_free(val);
                break;
            }
            case OBJ_LIST: {
                o = obj_create_list();
                uint32_t n = r_u32(f);
                for (uint32_t j = 0; j < n; j++) {
                    sds elem = r_sds(f);
                    adlist_push_tail((adlist *)o->ptr, elem);
                }
                break;
            }
            case OBJ_HASH: {
                o = obj_create_hash();
                uint32_t n = r_u32(f);
                dict *d = (dict *)o->ptr;
                for (uint32_t j = 0; j < n; j++) {
                    sds field = r_sds(f);
                    sds val   = r_sds(f);
                    dict_replace(d, field, val);
                    sds_free(field); sds_free(val);
                }
                break;
            }
            case OBJ_SET: {
                o = obj_create_set();
                uint32_t n = r_u32(f);
                dict *d = (dict *)o->ptr;
                for (uint32_t j = 0; j < n; j++) {
                    sds member = r_sds(f);
                    dict_add(d, member, NULL);
                    sds_free(member);
                }
                break;
            }
            case OBJ_ZSET: {
                o = obj_create_zset();
                uint32_t n = r_u32(f);
                zset *zs = (zset *)o->ptr;
                for (uint32_t j = 0; j < n; j++) {
                    sds member = r_sds(f);
                    uint8_t b[8]; fread(b, 1, 8, f);
                    uint64_t raw = 0;
                    for (int k = 0; k < 8; k++) raw = (raw << 8) | b[k];
                    double score; memcpy(&score, &raw, 8);
                    dict_replace(zs->dict, member, &score);
                    zsl_insert(zs->zsl, score, sds_dup(member));
                    sds_free(member);
                }
                break;
            }
            default:
                sds_free(key); fclose(f); return -1;
            }

            if (o) {
                dict_replace(db->keys, key, o);
                if (dl >= 0) db_set_expire(db, key, (int)SDS_LEN(key), dl);
            }
            sds_free(key);
        }
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Background save (fork-free: run in a thread)                        */
/* ------------------------------------------------------------------ */

static void *bgsave_fn(void *arg) {
    credish_store *store = (credish_store *)arg;
    credish_rwlock_rdlock(&store->lock);
    rdb_save(store);
    credish_rwlock_rdunlock(&store->lock);
    return NULL;
}

int rdb_bgsave(credish_store *store) {
    credish_thread_t t;
    if (credish_thread_create(&t, bgsave_fn, store) != 0)
        return -1;
    credish_thread_detach(t);
    return 0;
}
