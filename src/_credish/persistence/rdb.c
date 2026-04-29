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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#define RDB_MAGIC    "CREDISH_RDB\n"
#define RDB_VERSION  1
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

static void w_u8(FILE *f, uint8_t v) {
    fwrite(&v, 1, 1, f);
    g_crc = crc32_update(g_crc, &v, 1);
}

static void w_u32(FILE *f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8),  (uint8_t)v };
    fwrite(b, 1, 4, f);
    g_crc = crc32_update(g_crc, b, 4);
}

static void w_i64(FILE *f, int64_t v) {
    uint64_t u = (uint64_t)v;
    uint8_t b[8];
    for (int i = 7; i >= 0; i--) { b[i] = u & 0xFF; u >>= 8; }
    fwrite(b, 1, 8, f);
    g_crc = crc32_update(g_crc, b, 8);
}

static void w_blob(FILE *f, const char *data, uint32_t len) {
    w_u32(f, len);
    fwrite(data, 1, len, f);
    g_crc = crc32_update(g_crc, data, len);
}

static void w_sds(FILE *f, sds s) {
    w_blob(f, s, (uint32_t)SDS_LEN(s));
}

/* ------------------------------------------------------------------ */
/* Value serialisation                                                  */
/* ------------------------------------------------------------------ */

static void save_string(FILE *f, credishObject *o) {
    sds s = (sds)o->ptr;
    w_sds(f, s);
}

static void save_list(FILE *f, credishObject *o) {
    adlist *l = (adlist *)o->ptr;
    w_u32(f, (uint32_t)l->len);
    listNode *n = l->head;
    while (n) {
        w_sds(f, (sds)n->value);
        n = n->next;
    }
}

static void save_hash(FILE *f, credishObject *o) {
    dict *d = (dict *)o->ptr;
    w_u32(f, (uint32_t)dict_size(d));
    dictIterator *it = dict_iter_new(d);
    dictEntry *e;
    while ((e = dict_iter_next(it))) {
        w_sds(f, (sds)e->key);
        w_sds(f, (sds)e->v.val);
    }
    dict_iter_free(it);
}

static void save_set(FILE *f, credishObject *o) {
    dict *d = (dict *)o->ptr;
    w_u32(f, (uint32_t)dict_size(d));
    dictIterator *it = dict_iter_new(d);
    dictEntry *e;
    while ((e = dict_iter_next(it)))
        w_sds(f, (sds)e->key);
    dict_iter_free(it);
}

static void save_zset(FILE *f, credishObject *o) {
    typedef struct { dict *dict; struct zskiplist *zsl; } zset;
    zset *zs = (zset *)o->ptr;
    w_u32(f, (uint32_t)dict_size(zs->dict));
    dictIterator *it = dict_iter_new(zs->dict);
    dictEntry *e;
    while ((e = dict_iter_next(it))) {
        w_sds(f, (sds)e->key);
        double score = *(double *)e->v.val;
        uint64_t raw;
        memcpy(&raw, &score, 8);
        uint8_t b[8];
        for (int i = 7; i >= 0; i--) { b[i] = raw & 0xFF; raw >>= 8; }
        fwrite(b, 1, 8, f);
        g_crc = crc32_update(g_crc, b, 8);
    }
    dict_iter_free(it);
}

/* ------------------------------------------------------------------ */
/* Save                                                                 */
/* ------------------------------------------------------------------ */

int rdb_save(credish_store *s) {
    char path[600];
    rdb_path(path, sizeof(path), s->cfg.data_dir);
    char tmp[608];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;

    g_crc = 0;
    /* Magic + version */
    size_t mlen = strlen(RDB_MAGIC);
    fwrite(RDB_MAGIC, 1, mlen, f);
    g_crc = crc32_update(g_crc, RDB_MAGIC, mlen);
    w_u8(f, RDB_VERSION);

    for (int i = 0; i < CREDISH_DB_COUNT; i++) {
        credish_db *db = &s->dbs[i];
        if (dict_size(db->keys) == 0) continue;

        w_u8(f, SECTION_DB);
        w_u32(f, (uint32_t)i);
        w_u32(f, (uint32_t)dict_size(db->keys));

        dictIterator *it = dict_iter_new(db->keys);
        dictEntry    *e;
        while ((e = dict_iter_next(it))) {
            credishObject *o = (credishObject *)e->v.val;
            w_u8(f, (uint8_t)o->type);

            sds key = (sds)e->key;
            w_sds(f, key);

            /* Expiry */
            int64_t dl = db_get_expire(db, key, (int)SDS_LEN(key));
            if (dl >= 0) { w_u8(f, 1); w_i64(f, dl); }
            else          { w_u8(f, 0); }

            switch (o->type) {
            case OBJ_STRING: save_string(f, o); break;
            case OBJ_LIST:   save_list(f, o);   break;
            case OBJ_HASH:   save_hash(f, o);   break;
            case OBJ_SET:    save_set(f, o);     break;
            case OBJ_ZSET:   save_zset(f, o);    break;
            }
        }
        dict_iter_free(it);
    }

    w_u8(f, SECTION_EOF);
    uint32_t final_crc = g_crc;
    uint8_t cb[4] = { (uint8_t)(final_crc >> 24), (uint8_t)(final_crc >> 16),
                      (uint8_t)(final_crc >> 8),  (uint8_t)final_crc };
    fwrite(cb, 1, 4, f);
    fflush(f);
    fclose(f);
    rename(tmp, path);
    s->last_save_time = (int64_t)time(NULL);
    return 0;
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

int rdb_load(credish_store *s) {
    char path[600];
    rdb_path(path, sizeof(path), s->cfg.data_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char magic[12] = {0};
    size_t mlen = strlen(RDB_MAGIC);
    if (fread(magic, 1, mlen, f) != mlen || memcmp(magic, RDB_MAGIC, mlen) != 0) {
        fclose(f); return -1;
    }
    uint8_t ver = r_u8(f);
    if (ver != RDB_VERSION) { fclose(f); return -1; }

    while (1) {
        uint8_t section = r_u8(f);
        if (section == SECTION_EOF) break;
        if (section != SECTION_DB) { fclose(f); return -1; }

        int db_id = (int)r_u32(f);
        if (db_id < 0 || db_id >= CREDISH_DB_COUNT) { fclose(f); return -1; }
        credish_db *db = &s->dbs[db_id];
        uint32_t count = r_u32(f);

        for (uint32_t i = 0; i < count; i++) {
            int type = (int)r_u8(f);
            sds key  = r_sds(f);

            uint8_t has_exp = r_u8(f);
            int64_t dl = has_exp ? r_i64(f) : -1;

            credishObject *o = NULL;
            switch (type) {
            case OBJ_STRING: {
                sds val = r_sds(f);
                o = obj_create_string(val, (int)SDS_LEN(val));
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
                typedef struct { dict *dict; struct zskiplist *zsl; } zset;
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
    credish_store *s = (credish_store *)arg;
    pthread_rwlock_rdlock(&s->lock);
    rdb_save(s);
    pthread_rwlock_unlock(&s->lock);
    return NULL;
}

int rdb_bgsave(credish_store *s) {
    pthread_t t;
    return pthread_create(&t, NULL, bgsave_fn, s) == 0 ? 0 : -1;
}
