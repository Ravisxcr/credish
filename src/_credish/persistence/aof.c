/*
 * AOF persistence — append-only command log.
 *
 * Format: RESP-like inline records written by aof_append_len() in db.c.
 * On load, each record is parsed and re-executed against the db layer.
 *
 * Crash recovery:
 *   If credish.pid exists at open time, the previous session was unclean.
 *   In that case we do a full AOF replay (the pid file is removed on clean close).
 */

#include "aof.h"
#include "../db.h"
#include "../object.h"
#include "../sds.h"
#include "../adlist.h"
#include "../dict.h"
#include "../skiplist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define AOF_WRITE_BUFFER_SIZE (1024 * 1024)
#define AOF_REPLAY_BUFFER_SIZE (1024 * 1024)

static void aof_path(char *buf, size_t len, const char *data_dir) {
    snprintf(buf, len, "%s/credish.aof", data_dir);
}

int aof_open(credish_store *store) {
    char path[600];
    aof_path(path, sizeof(path), store->config.data_dir);
    store->aof_file = fopen(path, "ab");
    if (store->aof_file) {
        store->aof_write_buf = malloc(AOF_WRITE_BUFFER_SIZE);
        if (store->aof_write_buf)
            setvbuf(store->aof_file, store->aof_write_buf, _IOFBF, AOF_WRITE_BUFFER_SIZE);
    }
    return store->aof_file ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Parse helpers                                                        */
/* ------------------------------------------------------------------ */

/* Read one RESP bulk string $<len>\r\n<data>\r\n
 * Returns malloc'd buffer (caller frees) or NULL on error/EOF. */
static char *read_bulk(FILE *f, size_t *out_len) {
    char line[256];
    if (!fgets(line, sizeof(line), f)) return NULL;
    if (line[0] != '$') return NULL;
    int len = atoi(line + 1);
    if (len < 0) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    if ((int)fread(buf, 1, len, f) != len) { free(buf); return NULL; }
    buf[len] = '\0';
    /* consume \r\n */
    fgetc(f); fgetc(f);
    *out_len = (size_t)len;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Command replay                                                       */
/* ------------------------------------------------------------------ */

/*
 * Re-execute a parsed command array against the store.
 * Only mutating commands are stored in the AOF, so we implement
 * a minimal replay-only dispatcher here.
 */
static void replay_cmd(credish_store *store, int db_id, int argc, char **argv, size_t *argv_lens) {
    if (argc < 1) return;
    credish_db *db = store_select_db(store, db_id);
    if (!db) return;
    char *cmd = argv[0];

#define ARGC_MIN(n) if (argc < (n)) return

    if (credish_strcasecmp(cmd, "SET") == 0) {
        ARGC_MIN(3);
        int encoding = OBJ_ENCODING_RAW;
        for (int i = 3; i + 1 < argc; i++) {
            if (credish_strcasecmp(argv[i], "FMT") == 0) {
                encoding = atoi(argv[i + 1]);
                break;
            }
        }
        credishObject *o = obj_create_string_encoded(argv[2], (int)argv_lens[2], encoding);
        db_set(db, argv[1], (int)argv_lens[1], o, store);
        /* optional EX/PX */
        for (int i = 3; i + 1 < argc; i++) {
            if (credish_strcasecmp(argv[i], "EX") == 0) {
                int64_t sec = atoll(argv[i + 1]);
                int64_t dl = credish_now_ms() + sec * 1000LL;
                db_set_expire(db, argv[1], (int)argv_lens[1], dl);
                i++;
            } else if (credish_strcasecmp(argv[i], "PX") == 0) {
                int64_t ms = atoll(argv[i + 1]);
                int64_t dl = credish_now_ms() + ms;
                db_set_expire(db, argv[1], (int)argv_lens[1], dl);
                i++;
            }
        }
    } else if (credish_strcasecmp(cmd, "DEL") == 0) {
        for (int i = 1; i < argc; i++)
            db_del(db, argv[i], (int)argv_lens[i], store);
    } else if (credish_strcasecmp(cmd, "EXPIRE") == 0) {
        ARGC_MIN(3);
        int64_t sec = atoll(argv[2]);
        int64_t dl = credish_now_ms() + sec * 1000LL;
        db_set_expire(db, argv[1], (int)argv_lens[1], dl);
    } else if (credish_strcasecmp(cmd, "PERSIST") == 0) {
        ARGC_MIN(2);
        db_remove_expire(db, argv[1], (int)argv_lens[1]);
    } else if (credish_strcasecmp(cmd, "SELECT") == 0) {
        /* no-op during replay: db index tracked in AOF header per-command */
    } else if (credish_strcasecmp(cmd, "FLUSHDB") == 0) {
        /* wipe the current db */
        dict_free(db->keys);
        dict_free(db->expires);
        /* re-create empty dicts (reuse same dict_type from db.c via store_open path) */
    } else if (credish_strcasecmp(cmd, "INCRBY") == 0) {
        ARGC_MIN(3);
        long long val = 0;
        credishObject *o = db_lookup(db, argv[1], (int)argv_lens[1]);
        if (o && o->type == OBJ_STRING) {
            int vlen; char *vptr = obj_string_ptr(o, &vlen);
            char tmp[64]; int cplen = vlen < 63 ? vlen : 63;
            memcpy(tmp, vptr, (size_t)cplen); tmp[cplen] = '\0';
            val = strtoll(tmp, NULL, 10);
        }
        val += strtoll(argv[2], NULL, 10);
        char buf[24]; int blen = snprintf(buf, sizeof(buf), "%lld", val);
        credishObject *new_o = obj_create_string(buf, blen);
        if (new_o) db_set(db, argv[1], (int)argv_lens[1], new_o, store);
    } else if (credish_strcasecmp(cmd, "RPUSH") == 0) {
        ARGC_MIN(3);
        credishObject *o = db_lookup(db, argv[1], (int)argv_lens[1]);
        if (!o) {
            o = obj_create_list();
            if (o) db_set(db, argv[1], (int)argv_lens[1], o, store);
        }
        if (o && o->type == OBJ_LIST) {
            adlist *l = (adlist *)o->ptr;
            for (int i = 2; i < argc; i++) {
                sds v = sds_newlen(argv[i], argv_lens[i]);
                if (v) adlist_push_tail(l, v);
            }
        }
    } else if (credish_strcasecmp(cmd, "LPUSH") == 0) {
        ARGC_MIN(3);
        credishObject *o = db_lookup(db, argv[1], (int)argv_lens[1]);
        if (!o) {
            o = obj_create_list();
            if (o) db_set(db, argv[1], (int)argv_lens[1], o, store);
        }
        if (o && o->type == OBJ_LIST) {
            adlist *l = (adlist *)o->ptr;
            for (int i = 2; i < argc; i++) {
                sds v = sds_newlen(argv[i], argv_lens[i]);
                if (v) adlist_push_head(l, v);
            }
        }
    } else if (credish_strcasecmp(cmd, "ZADD") == 0) {
        ARGC_MIN(4);
        credishObject *o = db_lookup(db, argv[1], (int)argv_lens[1]);
        if (!o) {
            o = obj_create_zset();
            if (o) db_set(db, argv[1], (int)argv_lens[1], o, store);
        }
        if (o && o->type == OBJ_ZSET) {
            zset *zs = (zset *)o->ptr;
            for (int i = 2; i + 1 < argc; i += 2) {
                double score = strtod(argv[i], NULL);
                sds member = sds_newlen(argv[i + 1], argv_lens[i + 1]);
                if (!member) continue;
                double *oldp = dict_fetch_value(zs->dict, member);
                if (oldp) zsl_delete(zs->zsl, *oldp, member);
                dict_replace(zs->dict, member, &score);
                zsl_insert(zs->zsl, score, sds_dup(member));
                sds_free(member);
            }
        }
    } else if (credish_strcasecmp(cmd, "ZREM") == 0) {
        ARGC_MIN(3);
        credishObject *o = db_lookup(db, argv[1], (int)argv_lens[1]);
        if (o && o->type == OBJ_ZSET) {
            zset *zs = (zset *)o->ptr;
            for (int i = 2; i < argc; i++) {
                sds member = sds_newlen(argv[i], argv_lens[i]);
                if (!member) continue;
                double *score = dict_fetch_value(zs->dict, member);
                if (score) {
                    double old = *score;
                    zsl_delete(zs->zsl, old, member);
                    dict_delete(zs->dict, member);
                }
                sds_free(member);
            }
        }
    } else if (credish_strcasecmp(cmd, "HSET") == 0) {
        ARGC_MIN(4);
        credishObject *o = db_lookup(db, argv[1], (int)argv_lens[1]);
        if (!o) {
            o = obj_create_hash();
            if (o) db_set(db, argv[1], (int)argv_lens[1], o, store);
        }
        if (o && o->type == OBJ_HASH) {
            dict *d = (dict *)o->ptr;
            for (int i = 2; i + 1 < argc; i += 2) {
                sds field = sds_newlen(argv[i], argv_lens[i]);
                sds val = sds_newlen(argv[i + 1], argv_lens[i + 1]);
                if (field && val) dict_replace(d, field, val);
                sds_free(field);
                sds_free(val);
            }
        }
    } else if (credish_strcasecmp(cmd, "HDEL") == 0) {
        ARGC_MIN(3);
        credishObject *o = db_lookup(db, argv[1], (int)argv_lens[1]);
        if (o && o->type == OBJ_HASH) {
            dict *d = (dict *)o->ptr;
            for (int i = 2; i < argc; i++) {
                sds field = sds_newlen(argv[i], argv_lens[i]);
                if (field) dict_delete(d, field);
                sds_free(field);
            }
        }
    }
#undef ARGC_MIN
}

/* ------------------------------------------------------------------ */
/* Load                                                                 */
/* ------------------------------------------------------------------ */

int aof_load(credish_store *store) {
    char path[600];
    aof_path(path, sizeof(path), store->config.data_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char *replay_buf = malloc(AOF_REPLAY_BUFFER_SIZE);
    if (replay_buf)
        setvbuf(f, replay_buf, _IOFBF, AOF_REPLAY_BUFFER_SIZE);

    int db_id = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != '*') continue;
        int n = atoi(line + 1);
        if (n <= 0) continue;

        char **argv = calloc(n, sizeof(char *));
        size_t *argv_lens = calloc(n, sizeof(size_t));
        if (!argv || !argv_lens) {
            free(argv);
            free(argv_lens);
            break;
        }

        int ok = 1;
        for (int i = 0; i < n; i++) {
            argv[i] = read_bulk(f, &argv_lens[i]);
            if (!argv[i]) { ok = 0; break; }
        }
        if (ok) {
            /* The first "argv[0]" is the command; track SELECT for db_id */
            if (n >= 2 && credish_strcasecmp(argv[0], "SELECT") == 0)
                db_id = atoi(argv[1]);
            replay_cmd(store, db_id, n, argv, argv_lens);
        }
        for (int i = 0; i < n; i++) free(argv[i]);
        free(argv_lens);
        free(argv);
        if (!ok) break;
    }
    fclose(f);
    free(replay_buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Background fsync (everysec policy)                                  */
/* ------------------------------------------------------------------ */

void aof_fsync_bg(credish_store *store) {
    if (store->aof_file && store->config.aof_fsync == AOF_FSYNC_EVERYSEC)
        fflush(store->aof_file);
}
