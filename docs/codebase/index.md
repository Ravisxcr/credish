# C Source Guide

These pages describe each C implementation file under
[`src/_credish`](../../src/_credish), including its data structures, ownership
rules, public functions, and current implementation notes.

## Python Binding And Store

| Source file | Guide | Responsibility |
| --- | --- | --- |
| `credish_module.c` | [credish_module.md](credish_module.md) | Python extension entry point and ordinary command handlers |
| `db.c` | [db.md](db.md) | Logical databases, TTL storage, lifecycle, and AOF append helper |
| `server.c` | [server.md](server.md) | Persistence and fsync configuration parsing |
| `expire.c` | [expire.md](expire.md) | Background active-expiry worker |

## Values And Data Structures

| Source file | Guide | Responsibility |
| --- | --- | --- |
| `object.c` | [object.md](object.md) | Typed values and recursive destruction |
| `sds.c` | [sds.md](sds.md) | Binary-safe dynamic strings |
| `dict.c` | [dict.md](dict.md) | Generic hash table with incremental rehashing |
| `adlist.c` | [adlist.md](adlist.md) | Doubly linked list backing list values |
| `skiplist.c` | [skiplist.md](skiplist.md) | Score-ordered index for sorted sets |
| `sorted_set.c` | [sorted_set.md](sorted_set.md) | Python-facing sorted-set commands |
| `intset.c` | [intset.md](intset.md) | Integer-set representation currently not integrated into commands |
| `bufpool.c` | [bufpool.md](bufpool.md) | Slab allocator used by native data structures |

## Persistence

| Source file | Guide | Responsibility |
| --- | --- | --- |
| `persistence/aof.c` | [persistence/aof.md](persistence/aof.md) | Append-only-log loading and replay |
| `persistence/rdb.c` | [persistence/rdb.md](persistence/rdb.md) | Binary snapshot save and load |

