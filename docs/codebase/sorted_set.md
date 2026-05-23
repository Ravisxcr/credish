# Sorted-Set Commands: `sorted_set.c`

## Overview

[`src/_credish/sorted_set.c`](../../src/_credish/sorted_set.c) provides the
Python C-extension handlers for sorted-set commands. It uses the `zset` value
created by [`object.c`](object.md): a dictionary supports member lookup while a
[`skiplist`](skiplist.md) supports ordered traversal and ranks.

## Exported Commands

| Function | Python-style command | Purpose |
| --- | --- | --- |
| `py_zadd()` | `zadd` | Add or update scored members with `nx`, `xx`, `gt`, `lt`, and `ch` options. |
| `py_zrange()` / `py_zrevrange()` | `zrange` / `zrevrange` | Return rank ranges, optionally with scores. |
| `py_zrank()` / `py_zrevrank()` | `zrank` / `zrevrank` | Return forward or reverse rank. |
| `py_zscore()` | `zscore` | Look up one score. |
| `py_zrem()` | `zrem` | Remove members. |
| `py_zcard()` | `zcard` | Return member count. |
| `py_zrangebyscore()` | `zrangebyscore` | Iterate members in an inclusive score interval. |
| `py_zincrby()` | `zincrby` | Increment a score and return the new value. |

## Mutation Flow

`zset_add()` is the central mutation helper:

1. Look up the member in the score dictionary.
2. Apply `NX`, `XX`, `GT`, or `LT` conditions.
3. For a changed score, remove the old skip-list node.
4. Insert a new ordered node and replace the dictionary score.
5. Report whether a member was newly added or changed.

`zset_delete_member()` performs the corresponding deletion from both indexes.

## Python Conversion And Results

Keys accept Python `str` or `bytes`. Members accept `str`, `bytes`, `int`, or
`float` and are encoded as SDS bytes. Scores are Python floats and reject
`NaN`. Range results contain bytes, or `(bytes, float)` tuples when
`withscores` is requested.

## Locking And Persistence

Read commands hold the store read lock; mutations hold the write lock.
Successful changes append `ZADD` or `ZREM` operations through
[`db.c`](db.md) for AOF persistence.

## Current Scope

All sorted-set command handlers currently use logical database `0`, matching
the ordinary handlers in [`credish_module.c`](credish_module.md).

