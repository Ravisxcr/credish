# Python Extension Entry Point: `credish_module.c`

## Overview

[`src/_credish/credish_module.c`](../../src/_credish/credish_module.c) is the
main bridge between Python and Credish's native store. It registers the
`credish._credish` extension methods, converts Python arguments, controls
locking, and delegates storage work to [`db.c`](db.md) and the collection
modules.

## Store Handles

`py_open()` constructs a `credish_config`, calls `store_open()`, and returns a
`PyCapsule` wrapping `credish_store *`. `py_close()` closes the store and
replaces the capsule pointer with a sentinel so later destruction cannot free
the store twice.

`credish_get_store()` is exported for [`sorted_set.c`](sorted_set.md), allowing
its separate command handlers to resolve the same capsule.

## Value Conversion

| Helper | Conversion |
| --- | --- |
| `decode_key()` | Python `str` or `bytes` to borrowed bytes plus length. |
| `pyobj_to_sds()` | Python `str`, `bytes`, `int`, or `float` to an owned SDS value. |
| `now_ms_mod()` | Current Unix time in milliseconds for TTL commands. |

## Command Groups

| Group | Handlers |
| --- | --- |
| Store | `open`, `close`, `ping`, `flushdb`, `dbsize`, `select`, `save`, `bgsave` |
| Key and TTL | `delete`, `exists`, `expire`, `pexpire`, `persist`, `ttl`, `pttl`, `type` |
| Strings | `get`, `set`, `incrby` |
| Lists | `lpush`, `rpush`, `lrange`, `llen` |
| Sorted sets | Registered here; implemented in [`sorted_set.c`](sorted_set.md) |

String and list mutations work with `credishObject` instances created by
[`object.c`](object.md). Lists store SDS elements inside
[`adlist`](adlist.md) containers.

## Concurrency

The handlers take `s->lock` around native reads and writes. Mutations normally
use a write lock and lookups use a read lock. Native `db_lookup()` performs
lazy expiry deletion, so read handlers can currently trigger mutation while
holding the read lock.

## Persistence

`save()` and `bgsave()` call the RDB implementation. Commands such as `SET`,
`INCRBY`, `LPUSH`, and `RPUSH` call `aof_append()` after mutation.

## Current Behavioral Notes

- Ordinary handlers explicitly select logical database `0`; `py_select()` only
  validates its argument and returns success.
- `py_flushdb()` frees each database dictionary but does not rebuild empty
  dictionaries before later operations use them.
- Key deletion and TTL mutation handlers do not currently append corresponding
  AOF commands.

