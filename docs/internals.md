# Credish Internals

This document explains how Credish is structured below the Python client. The
project is intentionally shaped like a small embedded Redis: Python owns the
public API, while the C extension owns the key space, data structures, expiry,
and persistence.

## Architecture

```text
Python application
  |
  v
credish.client.CredishClient
  |
  v
credish._credish C extension
  |
  +-- store / DB layer        src/_credish/db.c
  +-- object layer            src/_credish/object.c
  +-- data structures         dict, sds, adlist, skiplist, intset
  +-- memory allocator        bufpool
  +-- expiry worker           expire
  +-- persistence             src/_credish/persistence/{rdb,aof}.c
  +-- cross-platform shims    platform.h
  +-- command exports         credish_module.c, sorted_set.c
```

The Python client keeps an opaque native handle returned by `_credish.open()`.
That handle is a Python capsule wrapping a `credish_store *`. Public methods in
`CredishClient` forward to C functions with this handle.

`platform.h` isolates every OS-specific primitive (rwlocks, mutexes,
one-time init, threads, monotonic time, sleep, and `fsync`) behind a
`credish_*` prefix, backed by pthreads on Linux/macOS and by `SRWLOCK`,
`CRITICAL_SECTION`, and `_beginthreadex` on Windows. The rest of the codebase
(db.c, expire.c, bufpool.c, persistence) is written against these shims
instead of pthreads directly, which is what lets Credish build on Windows.

## Store Lifecycle

The central native type is `credish_store` in `src/_credish/db.h`:

```c
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
```

`credish_config` (`src/_credish/server.h`) holds the data directory, the
persistence mode (`none`, `rdb`, `aof`, `hybrid`), the RDB save interval, and
the AOF fsync policy (`always`, `everysec`, `no`).

Opening a store:

1. Allocates and initializes a `credish_store`.
2. Creates 16 logical databases.
3. Initializes the global read/write lock.
4. Loads persisted state: AOF replay for `aof`/`hybrid` mode, RDB load for
   `rdb` mode.
5. Opens the AOF file (buffered with a 1 MB write buffer) when append-only
   persistence is enabled.
6. Starts the active expiry sweep thread.

Closing a store:

1. Stops the expiry sweep thread.
2. Writes a final RDB snapshot for `rdb` or `hybrid` modes.
3. Flushes and closes AOF, and frees the AOF write buffer.
4. Frees all DB dictionaries and objects.
5. Destroys the store lock.

`bgsave` does not fork like Redis; `rdb_bgsave()` (`persistence/rdb.c`) takes
the store's read lock and runs `rdb_save()` on a detached background thread
instead.

## Database Layout

Each logical database is a pair of dictionaries:

```c
typedef struct credish_db {
    dict *keys;     /* key -> credishObject* */
    dict *expires;  /* key -> int64_t unix-ms deadline */
    int   id;
} credish_db;
```

The `keys` dictionary owns the actual Redis-like objects. The `expires`
dictionary mirrors keys that have TTLs and stores absolute millisecond
deadlines. Keeping expiry separate makes ordinary key lookup fast and lets the
expiry worker scan only expiring keys.

## Object Model

Every stored value is wrapped in a `credishObject`:

```c
typedef struct credishObject {
    int   type;
    int   encoding;
    union {
        void           *ptr;   /* string (sds), or pointer to container */
        int64_t         ival;  /* small integer optimisation            */
    };
} credishObject;
```

Type tags match Redis-style type numbers:

| Type tag | Value | Backing structure |
| --- | ---: | --- |
| `OBJ_STRING` | `0` | `sds` |
| `OBJ_LIST` | `1` | `adlist` |
| `OBJ_SET` | `2` | `dict` |
| `OBJ_ZSET` | `3` | `zset` |
| `OBJ_HASH` | `4` | `dict` |

Strings are binary-safe SDS values. Lists are doubly linked lists. Sorted sets
combine a dictionary and skip list so Credish can support both fast member
lookup and ordered range traversal.

An integer-optimised set encoding, `intset` (`src/_credish/intset.c`), exists
in the tree as a placeholder for future `OBJ_SET` storage but is not yet wired
into `obj_create_set()` or the RDB/AOF formats.

### String Encoding

`encoding` only carries meaning for `OBJ_STRING` values. It records what
Python type a raw byte string round-trips to:

| Encoding tag | Value | Python type |
| --- | ---: | --- |
| `OBJ_ENCODING_RAW` | `0` | `bytes` |
| `OBJ_ENCODING_JSON` | `1` | JSON-serializable object (`dict`, `list`, ...) |
| `OBJ_ENCODING_STR` | `2` | `str` |
| `OBJ_ENCODING_INT` | `3` | `int` |
| `OBJ_ENCODING_FLOAT` | `4` | `float` |

`CredishClient` encodes non-`bytes` values before calling `set()` and passes
the matching tag as `value_encoding`; `get()` uses the stored tag to decode
the value back to its original Python type. The tag is persisted alongside
the value in RDB (since format version 2) and appended to AOF `SET` records
as a trailing `FMT <tag>` argument, so a reload preserves the original type.

## Buffer Pool and Pages

`src/_credish/bufpool.c` implements a small slab allocator used by fixed-size
internal structures such as `credishObject`, `dictEntry`, `dictIterator`,
`listNode`, `zset`, and many SDS allocations.

The goal is to avoid calling system `malloc()` and `free()` on hot paths for
small allocations. Instead, Credish allocates fixed-size slots from 4 KB pages.

### Size Classes

The allocator has 12 size classes:

```text
8, 16, 24, 32, 48, 64, 96, 128, 256, 512, 1024, 2048 bytes
```

An allocation request is rounded up to the first size class that can hold it.
Requests larger than 2048 bytes bypass the pool and use system `malloc()`.

### Page Layout

Each slab grows by allocating one 4096-byte page:

```text
+----------------------+--------------------------------------+
| page header          | fixed-size allocation slots           |
| struct page { next } | slot 0 | slot 1 | slot 2 | ...        |
+----------------------+--------------------------------------+
```

The page header is stored at the start of the page and links all pages owned by
that slab. Slot data starts immediately after the header:

```c
typedef struct page {
    struct page *next;
} page;
```

The slab stores:

```c
typedef struct {
    size_t          sz;
    free_slot      *free;
    char           *bump;
    char           *end;
    page           *pages;
    credish_mutex_t lock;
} slab;
```

Field meanings:

| Field | Purpose |
| --- | --- |
| `sz` | Slot size for this slab. |
| `free` | Singly linked list of recycled slots. |
| `bump` | Next unused byte in the current page. |
| `end` | One byte past the current page's usable range. |
| `pages` | Linked list of all pages allocated for this slab. |
| `lock` | Per-slab mutex for thread-safe allocation and free. |

### Allocation Flow

`bufpool_alloc(size)` works like this:

1. Initialize the global pool once with `credish_once()` (`pthread_once()` on
   Linux/macOS, `InitOnceExecuteOnce()` on Windows).
2. Find the smallest size class that can hold `size`.
3. If the request is too large, call `malloc(size)`.
4. Lock the selected slab.
5. If the slab has a recycled slot in `free`, pop and return it.
6. Otherwise, allocate from `bump`.
7. If the page is full, allocate a new 4 KB page and reset `bump` and `end`.
8. Unlock the slab and return the slot.

This gives O(1) allocation in the common case.

### Free Flow

`bufpool_free(ptr, size)` requires the same `size` value that was used for the
matching allocation. The allocator uses this size to find the correct slab.

For pooled sizes, the freed memory is overlaid with a small free-list node:

```c
typedef struct free_slot {
    struct free_slot *next;
} free_slot;
```

The slot is pushed onto the slab's `free` list and reused by a future allocation
of the same size class. Large allocations are returned directly to `free()`.

### Destroy Flow

`bufpool_destroy()` walks every slab's `pages` list and frees each 4 KB page. It
then clears the slab metadata and destroys the per-slab mutexes.

In normal store operation, individual fixed-size objects are returned to their
slab by type-specific free paths such as `obj_free()`, `dict_free()`, and
`adlist_free()`.

### Important Invariants

- The same size must be passed to `bufpool_alloc()` and `bufpool_free()`.
- Small freed allocations remain owned by the pool until reused or until
  `bufpool_destroy()` releases all pages.
- The allocator does not track per-pointer metadata, which keeps it fast but
  places correctness responsibility on callers.
- Each slab has its own mutex, so unrelated size classes can allocate
  concurrently.

## Simple Dynamic Strings

SDS is Credish's binary-safe string representation:

```c
typedef struct __attribute__((packed)) sdshdr {
    uint32_t len;
    uint32_t alloc;
    char     buf[];
} sdshdr;
```

The public `sds` value is a `char *` pointing at `buf`, while the header lives
immediately before it. This means an SDS can be used like a C string when data is
text, while still carrying its exact byte length for binary-safe operations.

SDS allocations use the buffer pool, including grows. When an SDS grows, a new
buffer is allocated, existing bytes are copied, and the old buffer is returned
to the pool with its original allocation size.

## Dictionary

`dict` is Credish's generic hash table. It is parameterized by a `dictType`
function table:

```c
typedef struct dictType {
    uint64_t (*hash)(const void *key);
    void    *(*key_dup)(void *key);
    void    *(*val_dup)(void *val);
    int      (*key_cmp)(const void *a, const void *b);
    void     (*key_free)(void *key);
    void     (*val_free)(void *val);
} dictType;
```

This lets the same dictionary implementation back multiple structures:

- key space: SDS key to `credishObject *`
- expiry table: SDS key to `int64_t *`
- hash object: SDS field to SDS value
- set object: SDS member to no value
- sorted set score dictionary: SDS member to `double *`

Internally the dictionary has two tables, `ht[0]` and `ht[1]`, so it can support
incremental rehashing. Entries are chained within buckets for collisions.

## Lists

Lists use `adlist`, a doubly linked list:

```c
typedef struct adlist {
    listNode *head;
    listNode *tail;
    size_t    len;
} adlist;
```

Each `listNode` stores a `void *value`, which is usually an SDS value for Redis
list commands. Nodes are allocated through the buffer pool. Values are freed via
the callback passed into `adlist_free()` or `adlist_delete_node()`.

## Sorted Sets

Sorted sets use two structures together:

```c
typedef struct zset {
    struct dict      *dict; /* member -> score */
    struct zskiplist *zsl;  /* ordered score/member view */
} zset;
```

The dictionary gives fast member lookup for commands such as `zscore`, `zrem`,
and score updates. The skip list gives ordered traversal for rank and range
commands. Skip list nodes store member SDS values, scores, backward links, and a
variable number of forward levels.

Members with equal scores are ordered lexicographically by member bytes.

## Expiry

Credish uses both lazy and active expiry.

Lazy expiry happens during lookup:

1. `db_lookup()` checks whether the key has an expired deadline.
2. If expired, the key is deleted from both `keys` and `expires`.
3. The lookup returns `NULL`, so the key behaves as missing.

Active expiry is handled by a background thread in `expire.c`, using a
sampling approach modeled on Redis rather than a full-table scan:

- Wakes every 100 ms (`SWEEP_INTERVAL_MS`) and takes the store write lock.
- For each database, repeatedly samples up to 20 keys (`SWEEP_SAMPLE_SIZE`)
  from a random offset in the `expires` dictionary and deletes the ones past
  their deadline.
- Keeps resampling the same database (up to `SWEEP_MAX_LOOPS` = 16 rounds per
  cycle) as long as at least 25% of the sampled keys were expired
  (`SWEEP_STOP_NUM` / `SWEEP_STOP_DEN`) — this is the signal that more expired
  keys are likely still present.
- Moves on once a round comes back mostly clean, or the database's `expires`
  dictionary empties out.

Sampling instead of a full scan keeps sweep cost roughly constant regardless
of how many keys a database holds, while the resample-on-high-hit-rate rule
still clears large batches of expired keys quickly after, e.g., a bulk
`EXPIRE`.

## Persistence

Credish has two persistence implementations, both under
`src/_credish/persistence/`.

### RDB

`rdb.c` writes a binary snapshot to `credish.rdb`. The file contains:

```text
"CREDISH_RDB\n"        magic (12 bytes)
version                uint8, currently 2
db sections            0xFE, db_id (uint32), key_count (uint32), then records
  key records            type (uint8), key (len-prefixed), has_expire (uint8),
                         [expire_ms (int64)], encoding (uint8, v2+),
                         type-specific value payload
EOF marker             0xFF
crc32 checksum         uint32, CRC-32/IEEE over everything above
```

`rdb_save()` writes to a `.tmp` file, fsyncs it and its parent directory, then
atomically renames it over `credish.rdb`. `rdb_load()` accepts both version 1
files (no per-key encoding byte, treated as `OBJ_ENCODING_RAW`) and version 2.
`rdb_bgsave()` runs `rdb_save()` on a detached thread under the store's read
lock rather than forking, since Credish is a library embedded in the caller's
process.

RDB supports strings, lists, hashes, sets, and sorted sets at the serialization
layer, though not every command group is currently exported through the public C
module.

### AOF

`aof.c` writes mutating commands to `credish.aof` using an inline, RESP-like
format:

```text
*<argc>\r\n
$<len>\r\n
<command>\r\n
$<len>\r\n
<arg>\r\n
...
```

The write file handle is opened in append mode with a 1 MB `setvbuf()` buffer.
String `SET` commands append the encoding tag as a trailing `FMT <tag>`
argument so replay can restore the value's Python-level type (see
[String Encoding](#string-encoding)).

On startup, AOF is parsed and replayed through `replay_cmd()`, a minimal
dispatcher that only understands the mutating commands Credish actually
appends (`SET`, `DEL`, `EXPIRE`, `PERSIST`, `SELECT`, `FLUSHDB`, `INCRBY`,
`LPUSH`, `RPUSH`, `ZADD`, `ZREM`). `SELECT` records track which logical
database subsequent commands in the file apply to.

The fsync behavior is controlled by `aof_fsync`:

| Mode | Behavior |
| --- | --- |
| `always` | Flush after each appended command. |
| `everysec` | Periodic flush via `aof_fsync_bg()`; the function exists but nothing currently calls it on a timer, so this mode presently behaves like `no`. |
| `no` | Let the OS decide when to flush. |

## Threading

The store uses a `credish_rwlock_t` — `pthread_rwlock_t` on Linux/macOS,
`SRWLOCK` on Windows, both wrapped by `platform.h`:

- Read operations can take the read lock.
- Writes take the write lock.
- The active expiry thread takes the write lock before deleting expired keys.
- Background RDB save (`rdb_bgsave()`) takes the read lock while it serializes
  the store on its own thread.

The buffer pool has independent per-slab mutexes (`credish_mutex_t`). That
keeps allocator metadata safe without forcing every small allocation through
one global mutex.

## Python Boundary

The C module exports functions through `credish_module.c` and
`sorted_set.c` (sorted-set commands are implemented and exported separately).
`_credish.open()` returns a Python capsule containing `credish_store *`, and
the capsule destructor closes the store if the user did not call `close()`
explicitly.

The public `CredishClient` in `credish/client.py` is a thin wrapper. New commands
usually need changes in three places:

1. C implementation and module export (`credish_module.c` or `sorted_set.c`).
2. Python wrapper method.
3. Type stub in `credish/_credish.pyi`.

Tests should cover the public wrapper rather than only the C function, because
the wrapper is the compatibility surface users call.
