# Configuration Parsing: `server.c`

## Overview

[`src/_credish/server.c`](../../src/_credish/server.c) converts configuration
strings supplied by the Python extension into the enums carried by
`credish_config` in [`src/_credish/server.h`](../../src/_credish/server.h).

## Configuration Model

```c
typedef struct credish_config {
    char data_dir[512];
    persist_mode_t persist_mode;
    int save_interval;
    aof_fsync_t aof_fsync;
} credish_config;
```

`py_open()` in [`credish_module.c`](credish_module.md) populates this struct,
and [`db.c`](db.md) uses it to select persistence behavior at store startup and
shutdown.

## Parsing Rules

| Input | Parsed persistence mode |
| --- | --- |
| `"none"` | `PERSIST_NONE` |
| `"rdb"` | `PERSIST_RDB` |
| `"aof"` | `PERSIST_AOF` |
| `NULL` or any other string | `PERSIST_HYBRID` |

| Input | Parsed AOF flush policy |
| --- | --- |
| `"always"` | `AOF_FSYNC_ALWAYS` |
| `"no"` | `AOF_FSYNC_NO` |
| `NULL` or any other string | `AOF_FSYNC_EVERYSEC` |

## Implementation Note

Parsing is permissive: unsupported option strings are silently interpreted as
the default behavior rather than being rejected as configuration errors.

