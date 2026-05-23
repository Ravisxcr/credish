# Dynamic Strings: `sds.c`

## Overview

[`src/_credish/sds.c`](../../src/_credish/sds.c) provides Credish's
binary-safe dynamic string representation, used for keys, string values,
container members, and serialized fields.

An `sds` is a `char *` pointing to bytes immediately after a compact header:

```c
typedef struct __attribute__((packed)) sdshdr {
    uint32_t len;
    uint32_t alloc;
    char buf[];
} sdshdr;
```

The stored length permits embedded NUL bytes even though the buffer also keeps
a trailing NUL for ordinary C-string interoperability.

## Allocation And Growth

SDS memory is obtained from the [`bufpool`](bufpool.md). `sds_grow()` cannot
use `realloc()` on pooled memory, so it allocates a larger buffer, copies the
header and content, then returns the old allocation to the pool. New capacity
doubles until it can fit the requested appended bytes.

## Operations

| Function | Behavior |
| --- | --- |
| `sds_newlen()` / `sds_new()` | Create a copied string. |
| `sds_empty()` / `sds_dup()` | Create an empty string or duplicate an SDS. |
| `sds_free()` / `sds_clear()` | Release content or reset its logical length. |
| `sds_grow()` / `sds_cat()` | Ensure capacity and append bytes. |
| `sds_catprintf()` | Format into a temporary buffer, then append. |
| `sds_cmp()` | Binary-safe lexical comparison. |

## Implementation Note

`sds_catprintf()` formats through a fixed 1024-byte temporary buffer. Formatted
text larger than that buffer is truncated before being appended.

