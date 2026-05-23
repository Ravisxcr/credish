# Stored Objects: `object.c`

## Overview

[`src/_credish/object.c`](../../src/_credish/object.c) creates and destroys
the values stored in a database keyspace. All values use the tagged
`credishObject` wrapper declared in
[`src/_credish/object.h`](../../src/_credish/object.h).

| Tag | Value kind | Internal representation |
| --- | --- | --- |
| `OBJ_STRING` | String | `sds` |
| `OBJ_LIST` | List | `adlist *` containing SDS values |
| `OBJ_HASH` | Hash | `dict *` mapping SDS fields to SDS values |
| `OBJ_SET` | Set | `dict *` containing SDS keys |
| `OBJ_ZSET` | Sorted set | `zset *` containing a dictionary and skip list |

## Constructors

String constructors either copy input bytes with `obj_create_string()` or
accept ownership of an existing SDS through `obj_steal_string()`.
`obj_create_string_int()` formats an integer as a stored SDS string.

Container constructors allocate an empty backing data structure:

- `obj_create_list()` creates an [`adlist`](adlist.md).
- `obj_create_hash()` and `obj_create_set()` create configured
  [`dict`](dict.md) instances.
- `obj_create_zset()` creates both a member-score dictionary and a
  [`skiplist`](skiplist.md) ordered index.

## Sorted-Set Dual Representation

The sorted-set dictionary maps a copied SDS member to a copied `double`
score. The skip list separately owns an SDS copy of each member in score
order. Mutation code must therefore update both views together.

## Destruction

`obj_free()` dispatches by type and recursively releases each backing
structure. Database key dictionaries install it as their `val_free` callback,
so deleting or replacing a database value also destroys the previous object.

## Helpers

`obj_is_string()` checks the type tag, while `obj_string_ptr()` exposes a
string's SDS bytes and length for command handlers.

## API Surface Note

The object layer provides hash and set storage representations even though the
currently exported Python command surface primarily exercises strings, lists,
and sorted sets.

