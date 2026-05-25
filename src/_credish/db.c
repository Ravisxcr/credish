#include "db.h"
#include "sds.h"
#include "expire.h"
#include "persistence/rdb.h"
#include "persistence/aof.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/* dictType for key space: SDS keys, credishObject* values            */
/* ------------------------------------------------------------------ */

static void obj_val_free(void *v)   { obj_free((credishObject *)v); }

static dictType keyspace_type = {
    .hash     = dict_hash_sds,
    .key_dup  = (void *(*)(void *))sds_dup,
    .val_dup  = NULL,
    .key_cmp  = dict_cmp_sds,
    .key_free = (void (*)(void *))sds_free,
    .val_free = obj_val_free,
};

/* dictType for expires: SDS keys, int64_t* values */
static void expires_val_free(void *v) { free(v); }
static void *expires_val_dup(void *v) {
    int64_t *p = malloc(sizeof(int64_t));
    if (p) *p = *(int64_t *)v;
    return p;
}
static dictType expires_type = {
    .hash     = dict_hash_sds,
    .key_dup  = (void *(*)(void *))sds_dup,
    .val_dup  = expires_val_dup,
    .key_cmp  = dict_cmp_sds,
    .key_free = (void (*)(void *))sds_free,
    .val_free = expires_val_free,
};

/* ------------------------------------------------------------------ */
/* Store lifecycle                                                     */
/* ------------------------------------------------------------------ */

credish_store *store_open(const credish_config *cfg) {
    credish_store *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = *cfg;

    for (int i = 0; i < CREDISH_DB_COUNT; i++) {
        s->dbs[i].id      = i;
        s->dbs[i].keys    = dict_create(&keyspace_type);
        s->dbs[i].expires = dict_create(&expires_type);
    }

    pthread_rwlock_init(&s->lock, NULL);

    /* Load persisted data */
    if (cfg->persist_mode == PERSIST_AOF || cfg->persist_mode == PERSIST_HYBRID)
        aof_load(s);
    else if (cfg->persist_mode == PERSIST_RDB)
        rdb_load(s);

    /* Open AOF for appending */
    if (cfg->persist_mode == PERSIST_AOF || cfg->persist_mode == PERSIST_HYBRID)
        aof_open(s);

    /* Start active expiry sweep */
    expire_sweep_start(s);

    s->last_save_time = (int64_t)time(NULL);
    return s;
}

void store_close(credish_store *s) {
    expire_sweep_stop(s);

    /* Final RDB save */
    if (s->cfg.persist_mode == PERSIST_RDB || s->cfg.persist_mode == PERSIST_HYBRID)
        rdb_save(s);

    if (s->aof_fp) {
        fflush(s->aof_fp);
        fclose(s->aof_fp);
        s->aof_fp = NULL;
    }
    free(s->aof_buf);
    s->aof_buf = NULL;

    for (int i = 0; i < CREDISH_DB_COUNT; i++) {
        dict_free(s->dbs[i].keys);
        dict_free(s->dbs[i].expires);
    }
    pthread_rwlock_destroy(&s->lock);
    free(s);
}

/* ------------------------------------------------------------------ */
/* DB accessors                                                        */
/* ------------------------------------------------------------------ */

credish_db *store_select_db(credish_store *s, int db_id) {
    if (db_id < 0 || db_id >= CREDISH_DB_COUNT) return NULL;
    return &s->dbs[db_id];
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

int db_is_expired(credish_db *db, const char *key, int keylen) {
    sds tmp = sds_newlen(key, (size_t)keylen);
    dictEntry *e = dict_find(db->expires, tmp);
    sds_free(tmp);
    if (!e) return 0;
    int64_t deadline = *(int64_t *)e->v.val;
    return now_ms() > deadline;
}

/* Lazy expiry: delete if past deadline; returns 1 if expired */
static int lazy_expire(credish_db *db, const char *key, int keylen) {
    if (!db_is_expired(db, key, keylen)) return 0;
    sds tmp = sds_newlen(key, (size_t)keylen);
    dict_delete(db->keys, tmp);
    dict_delete(db->expires, tmp);
    sds_free(tmp);
    return 1;
}

credishObject *db_lookup(credish_db *db, const char *key, int keylen) {
    if (lazy_expire(db, key, keylen)) return NULL;
    sds tmp = sds_newlen(key, (size_t)keylen);
    credishObject *o = dict_fetch_value(db->keys, tmp);
    sds_free(tmp);
    return o;
}

/* Same as db_lookup but signals "for write" intent (no semantic diff now) */
credishObject *db_lookup_write(credish_db *db, const char *key, int keylen) {
    return db_lookup(db, key, keylen);
}

int db_set(credish_db *db, const char *key, int keylen,
           credishObject *val, credish_store *s) {
    (void)s;
    sds k = sds_newlen(key, (size_t)keylen);
    int rc = dict_replace(db->keys, k, val);
    sds_free(k);
    return rc;
}

int db_del(credish_db *db, const char *key, int keylen, credish_store *s) {
    (void)s;
    sds tmp = sds_newlen(key, (size_t)keylen);
    int rc  = dict_delete(db->keys, tmp);
    dict_delete(db->expires, tmp);
    sds_free(tmp);
    return rc == 0 ? 1 : 0;
}

void db_set_expire(credish_db *db, const char *key, int keylen, int64_t deadline_ms) {
    sds tmp = sds_newlen(key, (size_t)keylen);
    dict_replace(db->expires, tmp, &deadline_ms);
    sds_free(tmp);
}

int64_t db_get_expire(credish_db *db, const char *key, int keylen) {
    sds tmp = sds_newlen(key, (size_t)keylen);
    int64_t *p = dict_fetch_value(db->expires, tmp);
    sds_free(tmp);
    return p ? *p : -1;
}

void db_remove_expire(credish_db *db, const char *key, int keylen) {
    sds tmp = sds_newlen(key, (size_t)keylen);
    dict_delete(db->expires, tmp);
    sds_free(tmp);
}

/* ------------------------------------------------------------------ */
/* AOF command append                                                  */
/* ------------------------------------------------------------------ */

void aof_append(credish_store *s, const char *cmd, int argc, const char **argv) {
    if (!s->aof_fp) return;
    /* RESP-like inline format: *N\r\n$len\r\ndata\r\n... */
    size_t cmdlen = strlen(cmd);
    size_t total = (size_t)snprintf(NULL, 0, "*%d\r\n", argc + 1);
    total += (size_t)snprintf(NULL, 0, "$%zu\r\n", cmdlen) + cmdlen + 2;
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]);
        total += (size_t)snprintf(NULL, 0, "$%zu\r\n", len) + len + 2;
    }

    char *record = malloc(total);
    if (!record) return;

    char *p = record;
    p += sprintf(p, "*%d\r\n", argc + 1);
    p += sprintf(p, "$%zu\r\n", cmdlen);
    memcpy(p, cmd, cmdlen); p += cmdlen;
    memcpy(p, "\r\n", 2); p += 2;

    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]);
        p += sprintf(p, "$%zu\r\n", len);
        memcpy(p, argv[i], len); p += len;
        memcpy(p, "\r\n", 2); p += 2;
    }

    fwrite(record, 1, (size_t)(p - record), s->aof_fp);
    free(record);

    if (s->cfg.aof_fsync == AOF_FSYNC_ALWAYS)
        fflush(s->aof_fp);
}
