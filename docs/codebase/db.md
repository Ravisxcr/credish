# Database Layer: `db.c`

## Overview

[`src/_credish/db.c`](../../src/_credish/db.c) is the storage-core layer for
Credish, an in-process Redis-like database. It manages:

- 16 logical databases.
- Key-to-value storage.
- Expiration timestamps.
- Store startup/shutdown.
- Persistence integration with RDB snapshots and AOF logs.

The public structs and functions are declared in
[`src/_credish/db.h`](../../src/_credish/db.h).

## Data Model

Each `credish_db` contains two hash tables:

```c
dict *keys;     /* key -> credishObject* */
dict *expires;  /* key -> int64_t deadline in milliseconds */
```

A key's value and its expiry are deliberately separate. For example:

```text
keys:     "session:1" -> credishObject("abc")
expires:  "session:1" -> 1760000000000
```

If a key is persistent, it exists in `keys` but not in `expires`.

Keys are stored as `sds`, Redis-style dynamically sized strings. Values are
`credishObject *`, which can represent strings, lists, hashes, sets, or sorted
sets; see [`src/_credish/object.h`](../../src/_credish/object.h).

## Dictionary Ownership

The start of [`src/_credish/db.c`](../../src/_credish/db.c) defines two
`dictType` configurations.

For the main keyspace:

```c
static dictType keyspace_type = {
    .key_dup  = sds_dup,
    .key_free = sds_free,
    .val_free = obj_val_free,
};
```

This means the dictionary:

- Makes its own copy of each key.
- Frees keys when deleted.
- Owns and frees stored `credishObject` values.

So when `db_set()` replaces an existing value, the old object is automatically destroyed by `dict_replace()`.

For expiration data:

```c
static dictType expires_type = {
    .key_dup  = sds_dup,
    .val_dup  = expires_val_dup,
    .key_free = sds_free,
    .val_free = free,
};
```

Here, the dictionary copies both the key and the `int64_t` expiry timestamp. That is why `db_set_expire()` may safely pass the address of its stack parameter to `dict_replace()`.

## Opening The Store

`store_open()` creates the entire database instance.

Its sequence is:

1. Allocate a zero-initialized `credish_store`.
2. Copy the supplied configuration.
3. Initialize all 16 logical databases, each with a `keys` dictionary and an `expires` dictionary.
4. Initialize the store read/write lock.
5. Restore persisted data:
   - AOF or hybrid mode calls `aof_load()`.
   - RDB mode calls `rdb_load()`.
6. Open the AOF file for future appends when needed.
7. Start the background expiry sweep thread.
8. Record the current save time.

The store contains shared process state including the databases, persistence handles, lock, and expiry thread metadata:

```c
typedef struct credish_store {
    credish_db dbs[16];
    credish_config cfg;
    pthread_rwlock_t lock;
    FILE *aof_fp;
    pthread_t sweep_thread;
    int sweep_running;
} credish_store;
```

## Closing The Store

`store_close()` performs the reverse cleanup:

1. Stop and join the expiry thread.
2. Write a final RDB snapshot in RDB or hybrid mode.
3. Flush and close the AOF file if it is open.
4. Free the dictionaries for all databases.
5. Destroy the lock.
6. Free the store itself.

Freeing `db->keys` also recursively frees all stored values because of `keyspace_type.val_free`.

## Selecting A Database

`store_select_db()` indexes one of the 16 logical databases:

```c
credish_db *store_select_db(credish_store *s, int db_id) {
    if (db_id < 0 || db_id >= CREDISH_DB_COUNT) return NULL;
    return &s->dbs[db_id];
}
```

This supports Redis-like logical database selection at the storage layer.
Currently, ordinary key commands in the Python extension select DB `0`
directly; `select()` validates a database identifier but does not change the
database used by later commands.

## Expiration Handling

The file supports lazy expiration as well as the background expiration thread
in [`src/_credish/expire.c`](../../src/_credish/expire.c).

`now_ms()` returns Unix time in milliseconds.

`db_is_expired()`:

- Looks up the key in the `expires` dictionary.
- Returns false if no expiry exists.
- Returns true once the current time is after its deadline.

`lazy_expire()` physically deletes an expired key from both dictionaries when
code tries to access it.

Thus a read performs this flow:

```text
GET key
  -> db_lookup()
     -> lazy_expire()
        -> expired? delete value and deadline, return missing
        -> otherwise retrieve value
```

This avoids returning expired values even before the background sweep has removed them.

## Reading Values

`db_lookup()` is the core read helper:

```c
credishObject *db_lookup(credish_db *db, const char *key, int keylen) {
    if (lazy_expire(db, key, keylen)) return NULL;
    sds tmp = sds_newlen(key, (size_t)keylen);
    credishObject *o = dict_fetch_value(db->keys, tmp);
    sds_free(tmp);
    return o;
}
```

It temporarily converts the supplied byte key into an `sds`, uses it to query the dictionary, then frees that temporary key.

`db_lookup_write()` currently delegates directly to `db_lookup()`. Its name
expresses the caller's intent: commands that may mutate a found container, such
as list or sorted-set operations, can use it even though there is no separate
behavior yet.

## Writing And Deleting Values

`db_set()` inserts or replaces a key:

```c
int rc = dict_replace(db->keys, k, val);
```

Important ownership rule: after a successful set, the dictionary owns `val`. When replacing a key, the previous object is freed.

`db_del()` deletes both:

- The stored object in `keys`.
- Any matching expiration entry in `expires`.

It returns `1` when a data key existed and was removed, or `0` when no data key was present.

## Expiration API

The expiry helper functions expose these operations:

- `db_set_expire()` installs or updates an absolute millisecond deadline.
- `db_get_expire()` returns a deadline, or `-1` for no expiry.
- `db_remove_expire()` makes a key persistent again.

These are used by commands corresponding to Redis behaviors such as `EXPIRE`, `PEXPIRE`, `TTL`, and `PERSIST`.

## AOF Persistence

`aof_append()` appends a command to the append-only file when AOF is enabled.

It serializes operations using Redis RESP-like records:

```text
*3\r\n
$3\r\nSET\r\n
$3\r\nkey\r\n
$5\r\nvalue\r\n
```

The function:

1. Does nothing unless `s->aof_fp` is open.
2. Writes the command and all arguments.
3. Immediately flushes the file when configured with `AOF_FSYNC_ALWAYS`.

On startup, [`src/_credish/persistence/aof.c`](../../src/_credish/persistence/aof.c)
reads those recorded commands and replays them to reconstruct the database.

## Locking

The header states that DB accessors expect the caller to hold the appropriate
lock.

`db.c` itself does not lock around individual operations. Higher-level command
handling and the expiry sweep use `credish_store.lock`. For example, the
expiry thread acquires a write lock before deleting expired entries. One detail
to keep in mind is that `db_lookup()` can delete an expired key during a lazy
expiry check, even when some current callers acquired a read lock.

## Summary

`db.c` is the central in-memory storage engine: it owns key/value and TTL dictionaries, provides CRUD operations with lazy expiration, wires startup and shutdown into persistence and background expiry, and emits AOF command records for recovery.
