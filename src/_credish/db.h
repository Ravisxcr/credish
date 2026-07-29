#ifndef CREDISH_DB_H
#define CREDISH_DB_H

#include "dict.h"
#include "object.h"
#include "server.h"
#include "platform.h"
#include <stdint.h>
#include <stdio.h>

#define CREDISH_DB_COUNT 16

typedef struct credish_db {
    dict *keys;     /* key -> credishObject* */
    dict *expires;  /* key -> int64_t (unix ms deadline) */
    int   id;
} credish_db;

typedef struct credish_store {
    credish_db      dbs[CREDISH_DB_COUNT];
    credish_config  config;
    credish_rwlock_t lock;

    /* AOF */
    FILE   *aof_file;
    char   *aof_write_buf;
    int64_t aof_sequence;

    /* RDB */
    int64_t last_save_time;  /* unix seconds */

    /* Active expiry sweep thread */
    credish_thread_t sweep_thread;
    int       sweep_thread_started;
    int       sweep_running;
} credish_store;

credish_store *store_open(const credish_config *cfg);
void           store_close(credish_store *store);

/* Internal db access (caller holds lock) */
credish_db    *store_select_db(credish_store *store, int db_id);
credishObject *db_lookup(credish_db *db, const char *key, int keylen);
credishObject *db_lookup_write(credish_db *db, const char *key, int keylen);
int            db_set(credish_db *db, const char *key, int keylen,
                      credishObject *val, credish_store *store);
int            db_del(credish_db *db, const char *key, int keylen,
                      credish_store *store);
void           db_set_expire(credish_db *db, const char *key, int keylen,
                             int64_t deadline_ms);
int64_t        db_get_expire(credish_db *db, const char *key, int keylen);
void           db_remove_expire(credish_db *db, const char *key, int keylen);
int            db_is_expired(credish_db *db, const char *key, int keylen);

/* AOF helpers (internal) */
void aof_append(credish_store *store, const char *cmd, int argc, const char **argv);
void aof_append_len(credish_store *store, const char *cmd, int argc,
                    const char **argv, const size_t *argv_lens);

#endif /* CREDISH_DB_H */
