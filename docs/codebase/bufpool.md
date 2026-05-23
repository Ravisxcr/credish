# Buffer Pool: `bufpool.c`

## Overview

[`src/_credish/bufpool.c`](../../src/_credish/bufpool.c) implements a small
thread-safe slab allocator for frequently allocated native objects. Its API is
declared in [`src/_credish/bufpool.h`](../../src/_credish/bufpool.h).

The allocator manages 4 KB pages split into fixed-size slots:

```text
8, 16, 24, 32, 48, 64, 96, 128, 256, 512, 1024, 2048 bytes
```

Requests above 2048 bytes go directly to `malloc()` and `free()`.

## Slab Layout

Each size class owns one `slab`:

```c
typedef struct {
    size_t sz;
    free_slot *free;
    char *bump;
    char *end;
    page *pages;
    pthread_mutex_t lock;
} slab;
```

Fresh allocations advance `bump` within the current page. Freed slots are
linked into `free` and reused before a new page is allocated.

## API Flow

| Function | Behavior |
| --- | --- |
| `bufpool_init()` | Initialize all slabs once with `pthread_once()`. |
| `bufpool_alloc(size)` | Round to a class, reuse a slot or grow its slab. |
| `bufpool_free(ptr, size)` | Return a slot to the correct free list or call `free()`. |
| `bufpool_destroy()` | Release pages and destroy slab mutexes. |

The original allocation size must be provided again to `bufpool_free()`, since
the pointer contains no independent size-class metadata.

## Consumers

The pool backs dictionary entries and iterators, list nodes, `credishObject`
instances, `zset` wrappers, and SDS allocations. The skip-list nodes themselves
use `malloc()` because their variable level count changes their allocation
size.

## Threading Note

Each slab has its own mutex, so allocations in separate size classes can
proceed independently. `bufpool_destroy()` is a final teardown operation; it
does not reset the `pthread_once()` initializer for reuse afterward.

