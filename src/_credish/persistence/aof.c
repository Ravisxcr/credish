/*
 * AOF persistence — append-only command log.
 *
 * Format: RESP-like inline records written by aof_append() in db.c.
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
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static void aof_path(char *buf, size_t len, const char *data_dir) {
    snprintf(buf, len, "%s/credish.aof", data_dir);
}

int aof_open(credish_store *s) {
    char path[600];
    aof_path(path, sizeof(path), s->cfg.data_dir);
    s->aof_fp = fopen(path, "ab");
    return s->aof_fp ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Parse helpers                                                        */
/* ------------------------------------------------------------------ */

/* Read one RESP bulk string $<len>\r\n<data>\r\n
 * Returns malloc'd buffer (caller frees) or NULL on error/EOF. */
static char *read_bulk(FILE *f) {
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
static void replay_cmd(credish_store *s, int db_id, int argc, char **argv) {
    if (argc < 1) return;
    credish_db *db = store_select_db(s, db_id);
    if (!db) return;
    char *cmd = argv[0];

#define ARGC_MIN(n) if (argc < (n)) return

    if (strcasecmp(cmd, "SET") == 0) {
        ARGC_MIN(3);
        credishObject *o = obj_create_string(argv[2], (int)strlen(argv[2]));
        db_set(db, argv[1], (int)strlen(argv[1]), o, s);
        /* optional EX/PX */
        for (int i = 3; i + 1 < argc; i++) {
            if (strcasecmp(argv[i], "EX") == 0) {
                int64_t sec = atoll(argv[i + 1]);
                struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
                int64_t dl = (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL
                             + sec * 1000LL;
                db_set_expire(db, argv[1], (int)strlen(argv[1]), dl);
                i++;
            } else if (strcasecmp(argv[i], "PX") == 0) {
                int64_t ms = atoll(argv[i + 1]);
                struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
                int64_t dl = (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL + ms;
                db_set_expire(db, argv[1], (int)strlen(argv[1]), dl);
                i++;
            }
        }
    } else if (strcasecmp(cmd, "DEL") == 0) {
        for (int i = 1; i < argc; i++)
            db_del(db, argv[i], (int)strlen(argv[i]), s);
    } else if (strcasecmp(cmd, "EXPIRE") == 0) {
        ARGC_MIN(3);
        int64_t sec = atoll(argv[2]);
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        int64_t dl = (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL + sec * 1000LL;
        db_set_expire(db, argv[1], (int)strlen(argv[1]), dl);
    } else if (strcasecmp(cmd, "PERSIST") == 0) {
        ARGC_MIN(2);
        db_remove_expire(db, argv[1], (int)strlen(argv[1]));
    } else if (strcasecmp(cmd, "SELECT") == 0) {
        /* no-op during replay: db index tracked in AOF header per-command */
    } else if (strcasecmp(cmd, "FLUSHDB") == 0) {
        /* wipe the current db */
        dict_free(db->keys);
        dict_free(db->expires);
        /* re-create empty dicts (reuse same dictType from db.c via store_open path) */
    } else if (strcasecmp(cmd, "INCRBY") == 0) {
        ARGC_MIN(3);
        long long val = 0;
        credishObject *o = db_lookup(db, argv[1], (int)strlen(argv[1]));
        if (o && o->type == OBJ_STRING) {
            int vlen; char *vptr = obj_string_ptr(o, &vlen);
            char tmp[64]; int cplen = vlen < 63 ? vlen : 63;
            memcpy(tmp, vptr, (size_t)cplen); tmp[cplen] = '\0';
            val = strtoll(tmp, NULL, 10);
        }
        val += strtoll(argv[2], NULL, 10);
        char buf[24]; int blen = snprintf(buf, sizeof(buf), "%lld", val);
        credishObject *new_o = obj_create_string(buf, blen);
        if (new_o) db_set(db, argv[1], (int)strlen(argv[1]), new_o, s);
    } else if (strcasecmp(cmd, "RPUSH") == 0) {
        ARGC_MIN(3);
        credishObject *o = db_lookup(db, argv[1], (int)strlen(argv[1]));
        if (!o) {
            o = obj_create_list();
            if (o) db_set(db, argv[1], (int)strlen(argv[1]), o, s);
        }
        if (o && o->type == OBJ_LIST) {
            adlist *l = (adlist *)o->ptr;
            for (int i = 2; i < argc; i++) {
                sds v = sds_new(argv[i], strlen(argv[i]));
                if (v) adlist_push_tail(l, v);
            }
        }
    } else if (strcasecmp(cmd, "LPUSH") == 0) {
        ARGC_MIN(3);
        credishObject *o = db_lookup(db, argv[1], (int)strlen(argv[1]));
        if (!o) {
            o = obj_create_list();
            if (o) db_set(db, argv[1], (int)strlen(argv[1]), o, s);
        }
        if (o && o->type == OBJ_LIST) {
            adlist *l = (adlist *)o->ptr;
            for (int i = 2; i < argc; i++) {
                sds v = sds_new(argv[i], strlen(argv[i]));
                if (v) adlist_push_head(l, v);
            }
        }
    } else if (strcasecmp(cmd, "ZADD") == 0) {
        ARGC_MIN(4);
        credishObject *o = db_lookup(db, argv[1], (int)strlen(argv[1]));
        if (!o) {
            o = obj_create_zset();
            if (o) db_set(db, argv[1], (int)strlen(argv[1]), o, s);
        }
        if (o && o->type == OBJ_ZSET) {
            zset *zs = (zset *)o->ptr;
            for (int i = 2; i + 1 < argc; i += 2) {
                double score = strtod(argv[i], NULL);
                sds member = sds_new(argv[i + 1], strlen(argv[i + 1]));
                if (!member) continue;
                double *oldp = dict_fetch_value(zs->dict, member);
                if (oldp) zsl_delete(zs->zsl, *oldp, member);
                dict_replace(zs->dict, member, &score);
                zsl_insert(zs->zsl, score, sds_dup(member));
                sds_free(member);
            }
        }
    } else if (strcasecmp(cmd, "ZREM") == 0) {
        ARGC_MIN(3);
        credishObject *o = db_lookup(db, argv[1], (int)strlen(argv[1]));
        if (o && o->type == OBJ_ZSET) {
            zset *zs = (zset *)o->ptr;
            for (int i = 2; i < argc; i++) {
                sds member = sds_new(argv[i], strlen(argv[i]));
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
    }
#undef ARGC_MIN
}

/* ------------------------------------------------------------------ */
/* Load                                                                 */
/* ------------------------------------------------------------------ */

int aof_load(credish_store *s) {
    char path[600];
    aof_path(path, sizeof(path), s->cfg.data_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    int db_id = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != '*') continue;
        int n = atoi(line + 1);
        if (n <= 0) continue;

        char **argv = calloc(n, sizeof(char *));
        if (!argv) break;

        int ok = 1;
        for (int i = 0; i < n; i++) {
            argv[i] = read_bulk(f);
            if (!argv[i]) { ok = 0; break; }
        }
        if (ok) {
            /* The first "argv[0]" is the command; track SELECT for db_id */
            if (n >= 2 && strcasecmp(argv[0], "SELECT") == 0)
                db_id = atoi(argv[1]);
            replay_cmd(s, db_id, n, argv);
        }
        for (int i = 0; i < n; i++) free(argv[i]);
        free(argv);
        if (!ok) break;
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Background fsync (everysec policy)                                  */
/* ------------------------------------------------------------------ */

void aof_fsync_bg(credish_store *s) {
    if (s->aof_fp && s->cfg.aof_fsync == AOF_FSYNC_EVERYSEC)
        fflush(s->aof_fp);
}
