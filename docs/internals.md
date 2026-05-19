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
  +-- data structures         dict, sds, adlist, skiplist
  +-- memory allocator        bufpool
  +-- expiry worker           expire
  +-- persistence             rdb, aof
```

The Python client keeps an opaque native handle returned by `_credish.open()`.
That handle is a Python capsule wrapping a `credish_store *`. Public methods in
`CredishClient` forward to C functions with this handle.

## Store Lifecycle

The central native type is `credish_store` in `src/_credish/db.h`:

```c
typedef struct credish_store {
    credish_db      dbs[CREDISH_DB_COUNT];
    credish_config  cfg;
    pthread_rwlock_t lock;

    FILE   *aof_fp;
    int64_t aof_seq;
    int64_t last_save_time;

    pthread_t sweep_thread;
    int       sweep_running;
} credish_store;
```

Opening a store:

1. Allocates and initializes a `credish_store`.
2. Creates 16 logical databases.
3. Initializes the global read/write lock.
4. Loads persisted state from AOF or RDB depending on configuration.
5. Opens the AOF file when append-only persistence is enabled.
6. Starts the active expiry sweep thread.

Closing a store:

1. Stops the expiry sweep thread.
2. Writes a final RDB snapshot for `rdb` or `hybrid` modes.
3. Flushes and closes AOF.
4. Frees all DB dictionaries and objects.
5. Destroys the store lock.

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
    int type;
    union {
        void   *ptr;
        int64_t ival;
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
    pthread_mutex_t lock;
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

1. Initialize the global pool once with `pthread_once()`.
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

Active expiry is handled by a background thread in `expire.c`:

- Wakes every 100 ms.
- Takes the store write lock.
- Scans each database's `expires` dictionary.
- Deletes up to 20 expired keys per database per pass.

The active worker prevents forgotten expired keys from accumulating when they
are not read again.

## Persistence

Credish has two persistence implementations.

### RDB

RDB writes a binary snapshot to `credish.rdb`. The file contains:

```text
CREDISH_RDB\n
version
db sections
key records
EOF marker
crc32 checksum
```

Each key record stores:

- object type
- key length and bytes
- optional expiry deadline
- type-specific value payload

RDB supports strings, lists, hashes, sets, and sorted sets at the serialization
layer, though not every command group is currently exported through the public C
module.

### AOF

AOF writes mutating commands to `credish.aof` using a RESP-like format:

```text
*<argc>
$<len>
<command>
$<len>
<arg>
...
```

On startup, AOF is parsed and replayed through a minimal replay dispatcher. This
reconstructs the database by applying stored mutations in order.

The fsync behavior is controlled by `aof_fsync`:

| Mode | Behavior |
| --- | --- |
| `always` | Flush after each appended command. |
| `everysec` | Intended periodic flush policy. |
| `no` | Let the OS decide when to flush. |

## Threading

The store uses a `pthread_rwlock_t`:

- Read operations can take the read lock.
- Writes take the write lock.
- The active expiry thread takes the write lock before deleting expired keys.

The buffer pool has independent per-slab mutexes. That keeps allocator metadata
safe without forcing every small allocation through one global mutex.

## Python Boundary

The C module exports functions through `credish_module.c`. `_credish.open()`
returns a Python capsule containing `credish_store *`, and the capsule
destructor closes the store if the user did not call `close()` explicitly.

The public `CredishClient` in `credish/client.py` is a thin wrapper. New commands
usually need changes in three places:

1. C implementation and module export.
2. Python wrapper method.
3. Type stub in `credish/_credish.pyi`.

Tests should cover the public wrapper rather than only the C function, because
the wrapper is the compatibility surface users call.
