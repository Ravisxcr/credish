# Errors and Limitations

## Exceptions

Credish exports these exception classes:

```python
from credish import CredishError, ResponseError, DataError, WrongTypeError
```

Common error behavior:

- Invalid key types raise `TypeError`.
- `set()` accepts `bytes`, `str`, `bool`, `int`, `float`, `bytearray`,
  `memoryview`, `None`, and JSON-compatible `list`/`dict` values. `bool`,
  `None`, `list`, and `dict` are stored as JSON; non-string `dict` keys inside
  such a value raise `DataError`. Any other type (e.g. a tuple or an
  arbitrary object) raises `DataError`, not `TypeError`.
- Wrong-type operations raise a plain `TypeError` containing `"WRONGTYPE"` in
  the current C extension. The `WrongTypeError` exception class is exported
  from `credish` but is not currently raised by any operation — it is reserved
  for a future error-type pass.
- `set(..., nx=True, xx=True)` raises `DataError`.
- `select()` and the `db=` constructor argument raise `DataError` for a
  non-integer database index and `ValueError` for an integer outside `0..15`.
- `incrby()` raises `TypeError` if `amount` is not an integer, and `ValueError`
  if the currently stored value is not integer-like.

## Wrapper-Only Methods

Some `CredishClient` methods are already present in Python as part of the
planned Redis-compatible API, but their corresponding C extension methods are
not currently exported.

Wrapper-only string/counter methods:

```text
getset, setnx, setex, psetex, mset, mget, append, strlen, incr, decr, decrby
```

Wrapper-only list methods:

```text
lpop, rpop, lindex, lset, lrem, ltrim
```

Wrapper-only key-space methods:

```text
keys
```

Calling these methods in the current build will raise `AttributeError` from
`credish._credish`.

## Planned Set Commands

Set methods are present in the Python wrapper as part of the planned
Redis-compatible API, but they are not currently exported by the C extension.
Hash methods (`hset`, `hget`, `hmset`, `hmget`, `hdel`, `hexists`, `hgetall`,
`hkeys`, `hvals`, `hlen`, `hincrby`) are fully implemented — see
[API Reference: Hash Commands](api-reference.md#hash-commands).

Planned set methods:

```text
sadd, srem, smembers, sismember, scard, sunion, sinter, sdiff
```

## Current Limitations

- The set command group is planned but not currently implemented in the C
  extension.
- Several Redis string/list/counter/key-space wrappers exist in Python before
  their C extension counterparts (see [Wrapper-Only Methods](#wrapper-only-methods)).
- This is an in-process cache, not a network Redis server.
- Credish builds on Linux and macOS with a POSIX-thread-capable C compiler,
  and on Windows using native threading APIs (`SRWLOCK`, `CRITICAL_SECTION`,
  `_beginthreadex`) behind the same internal interface — see
  [Internals: Architecture](internals.md#architecture).
- The AOF `everysec` fsync policy is not currently wired to a periodic timer,
  so it behaves like `no` until a background flush is added.
- The integer-optimised `intset` encoding exists in the C source but is not
  yet used by `OBJ_SET` storage or the persistence formats.
- Redis compatibility focuses on common command semantics, not complete Redis
  protocol or server behavior.
