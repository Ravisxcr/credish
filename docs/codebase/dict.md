# Dictionary: `dict.c`

## Overview

[`src/_credish/dict.c`](../../src/_credish/dict.c) implements the generic hash
table used for database keys, expirations, hashes, sets, and the member-score
index of sorted sets. Types and callbacks are declared in
[`src/_credish/dict.h`](../../src/_credish/dict.h).

## Generic Ownership

Every dictionary is configured with a `dictType` callback table:

```c
typedef struct dictType {
    uint64_t (*hash)(const void *key);
    void *(*key_dup)(void *key);
    void *(*val_dup)(void *val);
    int (*key_cmp)(const void *a, const void *b);
    void (*key_free)(void *key);
    void (*val_free)(void *val);
} dictType;
```

This lets one implementation own `sds -> credishObject *`,
`sds -> int64_t *`, `sds -> sds`, and `sds -> double *` mappings correctly.
The included `dict_hash_sds()` and `dict_cmp_sds()` helpers support SDS keys.

## Table And Rehashing

A `dict` contains two bucket arrays. Normally only `ht[0]` is active. When its
load reaches one entry per bucket, `dict_expand()` creates `ht[1]` at twice
the size and sets `rehash_idx`.

Ordinary lookups and mutations then call `dict_rehash_step()`, which moves one
non-empty bucket at a time. When the old table is empty, the new table becomes
`ht[0]`.

## Operations

| Function | Behavior |
| --- | --- |
| `dict_create()` / `dict_free()` | Allocate or recursively release a dictionary. |
| `dict_add()` | Insert only when a key is absent. |
| `dict_replace()` | Replace a value or insert a new entry. |
| `dict_find()` / `dict_fetch_value()` | Read an entry or its stored value. |
| `dict_delete()` | Remove one entry and invoke ownership callbacks. |
| `dict_size()` | Count entries across active and rehash tables. |

## Iterators

`dict_iter_new()` increments `d->iterators`. While an iterator exists, rehash
steps pause so traversal sees stable bucket chains. Iterators can walk both
tables when rehashing was already in progress.

## Allocation

The dictionary struct, entries, and iterators use the
[buffer pool](bufpool.md); bucket arrays are allocated with `calloc()`.

