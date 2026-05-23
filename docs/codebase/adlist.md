# Linked List: `adlist.c`

## Overview

[`src/_credish/adlist.c`](../../src/_credish/adlist.c) implements the
doubly-linked list used inside `OBJ_LIST` values. Its declarations live in
[`src/_credish/adlist.h`](../../src/_credish/adlist.h).

```c
typedef struct adlist {
    listNode *head;
    listNode *tail;
    size_t    len;
} adlist;
```

Each node stores an untyped `void *value`; list commands currently place `sds`
string values in those nodes.

## Allocation And Ownership

Both `adlist` and `listNode` instances are allocated through
[`bufpool.c`](bufpool.md). The list does not know the concrete value type, so
cleanup functions accept a `free_val` callback.

- `adlist_free()` frees every node and optionally every value.
- `adlist_delete_node()` optionally frees the removed node's value.
- `adlist_pop_head()` and `adlist_pop_tail()` return the value to the caller;
  they free only the detached node.

`obj_free()` supplies `sds_free` when destroying a stored list object.

## Operations

| Function | Behavior |
| --- | --- |
| `adlist_create()` | Allocate an empty list. |
| `adlist_push_head()` / `adlist_push_tail()` | Insert an item at either end. |
| `adlist_pop_head()` / `adlist_pop_tail()` | Detach and return an endpoint value. |
| `adlist_index()` | Find a zero-based item; negative indexes walk backward from the tail. |
| `adlist_delete_node()` | Unlink a known node. |
| `adlist_rem()` | Remove matching values from the head, tail, or throughout the list. |

`adlist_rem()` follows Redis-style count direction: positive counts search from
the head, negative counts search from the tail, and zero removes every match.

## Usage

List objects are created in [`object.c`](object.md), modified by
[`credish_module.c`](credish_module.md), and serialized by
[`persistence/rdb.c`](persistence/rdb.md).

## Implementation Note

The push functions have no return value. If allocation of a new node fails,
they silently leave the list unchanged, so their callers cannot currently
surface an allocation error.

