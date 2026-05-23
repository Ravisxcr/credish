# Append-Only Persistence: `aof.c`

## Overview

[`src/_credish/persistence/aof.c`](../../../src/_credish/persistence/aof.c)
opens and replays the append-only file at `data_dir/credish.aof`. Command
records are written by `aof_append()` in [`db.c`](../db.md).

## File Format

Commands use a RESP-like sequence of bulk strings:

```text
*3\r\n
$3\r\nSET\r\n
$3\r\nkey\r\n
$5\r\nvalue\r\n
```

`read_bulk()` parses one `$length` value into a temporary heap allocation.
`aof_load()` reads command arrays and passes them to `replay_cmd()`.

## Replay Dispatch

The replay dispatcher reconstructs state for these mutation families:

| Commands | Replay behavior |
| --- | --- |
| `SET`, `INCRBY` | Create or replace string objects. |
| `DEL`, `EXPIRE`, `PERSIST` | Update keys or their TTL metadata. |
| `LPUSH`, `RPUSH` | Create or extend list objects. |
| `ZADD`, `ZREM` | Maintain sorted-set dictionary and skip-list indexes. |
| `SELECT` | Change the tracked database identifier for following records. |
| `FLUSHDB` | Attempts to remove data from the selected database. |

## Opening And Flushing

| Function | Behavior |
| --- | --- |
| `aof_open()` | Open `credish.aof` in append-binary mode. |
| `aof_load()` | Replay existing records into an initialized store. |
| `aof_fsync_bg()` | Flush the `FILE *` when policy is `AOF_FSYNC_EVERYSEC`. |

`AOF_FSYNC_ALWAYS` flushing occurs in `db.c` immediately after writing a
command.

## Current Behavioral Notes

- The record writer uses `strlen()` for arguments, so AOF persistence is not
  binary-safe for embedded NUL bytes even though in-memory SDS strings are.
- Current `SET` command logging writes key and value only; expiry options set
  during a Python `set(..., ex=.../px=...)` call are not represented in its
  appended record.
- The `FLUSHDB` replay branch frees dictionaries without recreating them,
  matching the analogous command-handler issue.

