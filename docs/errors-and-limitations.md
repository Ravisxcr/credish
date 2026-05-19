# Errors and Limitations

## Exceptions

Credish exports these exception classes:

```python
from credish import CredishError, ResponseError, DataError, WrongTypeError
```

Common error behavior:

- Invalid key types raise `TypeError`.
- Invalid string values, such as dictionaries, lists, tuples, arbitrary objects,
  or `None`, raise `TypeError`.
- Wrong-type operations raise a `TypeError` containing `"WRONGTYPE"` in the
  current C extension.
- `set(..., nx=True, xx=True)` raises `DataError`.
- `select()` with an invalid database index raises `ValueError`.
- `incrby()` requires an integer amount and an integer-like stored value.

## Wrapper-Only Methods

Some `CredishClient` methods are already present in Python as part of the
planned Redis-compatible API, but their corresponding C extension methods are
not currently exported.

Wrapper-only string methods:

```text
getset, setnx, setex, psetex, mset, mget, append, strlen
```

Wrapper-only list methods:

```text
lpop, rpop, lindex, lset, lrem, ltrim
```

Calling these methods in the current build will raise `AttributeError` from
`credish._credish`.

## Planned Hash and Set Commands

Hash and set methods are present in the Python wrapper as part of the planned
Redis-compatible API, but they are not currently exported by the C extension.

Planned hash methods:

```text
hset, hget, hmset, hmget, hdel, hexists, hgetall, hkeys, hvals, hlen, hincrby
```

Planned set methods:

```text
sadd, srem, smembers, sismember, scard, sunion, sinter, sdiff
```

## Current Limitations

- Hash and set command groups are planned but not currently implemented in the
  C extension.
- Several Redis string/list wrappers exist in Python before their C extension
  counterparts.
- This is an in-process cache, not a network Redis server.
- The current target platform is POSIX-like systems with pthread support.
- Redis compatibility focuses on common command semantics, not complete Redis
  protocol or server behavior.
