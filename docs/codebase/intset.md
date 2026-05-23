# Integer Set: `intset.c`

## Overview

[`src/_credish/intset.c`](../../src/_credish/intset.c) implements a compact,
sorted array of unique `int64_t` values. The header describes it as an
integer-optimized set encoding intended for sets whose members are all
integers.

```c
typedef struct intset {
    int64_t *contents;
    size_t length;
} intset;
```

## Operations

| Function | Behavior |
| --- | --- |
| `intset_create()` / `intset_free()` | Allocate or destroy the array wrapper. |
| `intset_find()` | Test membership through binary search. |
| `intset_add()` | Insert a missing integer while preserving sorted order. |
| `intset_remove()` | Remove an existing integer and compact the tail. |

`intset_search()` returns both membership and the insertion position. Reads are
logarithmic; insertions and removals are linear once `memmove()` shifts array
contents.

## Memory Model

The wrapper is created with `calloc()`. Insertions grow `contents` with
`realloc()`. Removing items reduces `length`, but does not shrink the allocated
array.

## Current Integration

This module is currently standalone. `OBJ_SET` in [`object.c`](object.md) is
backed by a dictionary of SDS members, and no Python-facing command handler
uses `intset`.

