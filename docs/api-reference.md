# API Reference

This page documents the command surface currently backed by the C extension,
plus the `session()`/`client()` helpers, which are Python-level wrappers over
the same store handle rather than separate C commands.

## Server Commands

### `ping() -> str`

Returns `"PONG"` when the store is available.

```python
client.ping()
```

### `flushdb() -> bool`

Removes all keys from the active database.

```python
client.flushdb()
```

### `dbsize() -> int`

Returns the number of keys in the active database.

```python
client.dbsize()
```

### `select(db: int) -> bool`

Switches the client to another logical database. Valid indexes are `0` through
`15`.

```python
client.select(1)
```

### `save() -> bool`

Writes a synchronous RDB snapshot.

```python
client.save()
```

### `bgsave() -> bool`

Triggers a background RDB snapshot.

```python
client.bgsave()
```

### `session(db: int | None = None) -> CredishClient`

Returns another `CredishClient` that shares the same underlying native store
(no new store is opened) but tracks its own selected logical database. This
mirrors opening multiple `redis-py` clients against one server. Closing a
session obtained this way does not close the underlying store; only the
client that originally opened it does that.

```python
main = CredishClient()
worker = main.session(db=1)
worker.set("key", "value")   # written to db 1
main.get("key")               # None — main is still on db 0
```

### `client(db: int | None = None) -> CredishClient`

`redis-py`-style alias for `session()`.

```python
client.client(db=2)
```

## Key and Expiry Commands

### `delete(*keys: str) -> int`

Deletes one or more keys and returns the number of keys removed.

```python
client.delete("session", "cache")
```

### `exists(*keys: str) -> int`

Returns the number of provided keys that exist.

```python
client.exists("session")
```

### `expire(key: str, seconds: int) -> bool`

Sets a key expiry in seconds. Returns `False` for missing keys.

```python
client.expire("session", 60)
```

### `pexpire(key: str, milliseconds: int) -> bool`

Sets a key expiry in milliseconds.

```python
client.pexpire("session", 1500)
```

### `persist(key: str) -> bool`

Removes an expiry from a key. Returns `False` when the key has no expiry.

```python
client.persist("session")
```

### `ttl(key: str) -> int`

Returns the remaining lifetime in seconds:

- `>= 0`: remaining seconds
- `-1`: key exists but has no expiry
- `-2`: key does not exist

```python
client.ttl("session")
```

### `pttl(key: str) -> int`

Returns the remaining lifetime in milliseconds, using the same sentinel values
as `ttl()`.

```python
client.pttl("session")
```

### `type(key: str) -> str`

Returns the stored type name for a key, such as `"string"`, `"list"`, `"zset"`,
or `"none"` for missing keys.

```python
client.type("leaders")
```

## String Commands

### `set(key, value, ex=None, px=None, nx=False, xx=False) -> bool | None`

Stores a value under `key`. `value` may be `bytes`, `bytearray`,
`memoryview`, `str`, `int`, `float`, `bool`, `None`, or a JSON-compatible
`list`/`dict` (values nested inside a `list`/`dict` must themselves be
JSON-compatible, and any `dict` keys must be `str`). Non-`bytes` values are
tagged with an encoding on write so `get(key, native=True)` (below) can decode
them back to their original Python type. Any other value type (e.g. a
`tuple` or an arbitrary object) raises `DataError`.

Options:

- `ex`: expiry in seconds
- `px`: expiry in milliseconds
- `nx`: only set if the key does not exist
- `xx`: only set if the key already exists

`nx` and `xx` are mutually exclusive. If an `nx` or `xx` condition is not met,
the command returns `None`.

```python
client.set("name", "credish")
client.set("lock", "1", ex=30, nx=True)
client.set("existing", "new-value", xx=True)
client.set("profile", {"name": "ann", "age": 30})
```

### `get(key, native=False) -> Any`

Returns the stored value as `bytes`, or `None` if the key is missing.

With `native=True`, the value is decoded back to the Python type it was
stored as (`str`, `int`, `float`, or a JSON-decoded `list`/`dict`/`bool`/
`None`); values written as plain `bytes` are returned unchanged.

```python
client.set("name", "credish")
client.get("name")                 # b"credish"

client.set("count", 3)
client.get("count")                # b"3"
client.get("count", native=True)   # 3

client.set("profile", {"name": "ann"})
client.get("profile", native=True) # {"name": "ann"}
```

### `incrby(key: str, amount: int) -> int`

Increments an integer string by `amount` and returns the new integer value.
Negative amounts decrement.

```python
client.set("counter", "10")
client.incrby("counter", 5)  # 15
client.incrby("counter", -3) # 12
```

### Convenience String Wrappers

`CredishClient` also exposes `incr()`, `decr()`, and `decrby()` as Python-level
convenience methods over the C command set. See
[Errors and Limitations](errors-and-limitations.md) for wrapper-only methods.

## List Commands

### `lpush(key: str, *values) -> int`

Prepends values to a list and returns the new length.

```python
client.lpush("tasks", "c", "b", "a")
client.lrange("tasks", 0, -1)  # [b"a", b"b", b"c"]
```

### `rpush(key: str, *values) -> int`

Appends values to a list and returns the new length.

```python
client.rpush("tasks", "a", "b", "c")
```

### `lrange(key: str, start: int, stop: int) -> list[bytes]`

Returns an inclusive list slice. Negative indexes are supported, including
`stop=-1` for the end of the list.

```python
client.lrange("tasks", 0, -1)
client.lrange("tasks", 1, 3)
```

### `llen(key: str) -> int`

Returns the list length.

```python
client.llen("tasks")
```

## Sorted Set Commands

Sorted sets store unique members ordered by score. Members with equal scores are
ordered lexicographically by member bytes.

### `zadd(key, mapping, nx=False, xx=False, gt=False, lt=False, ch=False) -> int`

Adds or updates members in a sorted set.

Options:

- `nx`: only add new members
- `xx`: only update existing members
- `gt`: only update if the new score is greater
- `lt`: only update if the new score is lower
- `ch`: count changed scores as well as newly added members

Scores must be finite numbers. `NaN` is rejected.

```python
client.zadd("leaders", {"ann": 10, "bob": 20})
client.zadd("leaders", {"ann": 30}, ch=True)
```

### `zrange(key, start, stop, withscores=False) -> list`

Returns members ordered from lowest to highest score.

```python
client.zrange("leaders", 0, -1)
client.zrange("leaders", 0, -1, withscores=True)
```

### `zrevrange(key, start, stop, withscores=False) -> list`

Returns members ordered from highest to lowest score.

```python
client.zrevrange("leaders", 0, -1)
```

### `zrank(key, member) -> int | None`

Returns the zero-based ascending rank for a member, or `None` if missing.

```python
client.zrank("leaders", "ann")
```

### `zrevrank(key, member) -> int | None`

Returns the zero-based descending rank for a member, or `None` if missing.

```python
client.zrevrank("leaders", "ann")
```

### `zscore(key, member) -> float | None`

Returns a member score, or `None` if the member is missing.

```python
client.zscore("leaders", "ann")
```

### `zrem(key, *members) -> int`

Removes members and returns the number removed.

```python
client.zrem("leaders", "ann", "missing")
```

### `zcard(key) -> int`

Returns the number of members in the sorted set.

```python
client.zcard("leaders")
```

### `zrangebyscore(key, min, max, withscores=False) -> list`

Returns members with scores in the inclusive range `[min, max]`.

```python
client.zrangebyscore("leaders", 10, 20, withscores=True)
```

### `zincrby(key, amount, member) -> float`

Increments a member score and returns the new score.

```python
client.zincrby("leaders", 5, "ann")
```

## Hash Commands

Hashes store field/value pairs under a single key, similar to a Python `dict`
of `bytes`. See the [Redis hashes documentation](https://redis.io/docs/latest/develop/data-types/hashes/)
for the general data type semantics.

Each field's *value* is tagged with its original Python type
(`str`/`bytes`/`int`/`float`) when written by `hset`/`hmset`. By default,
getters return the raw `bytes` payload as always; pass `native=True` to
`hget`/`hmget`/`hgetall`/`hvals`/`hkeys` to decode values (and, for
`hgetall`/`hkeys`, field names too) back to their original type instead —
mirroring `get(key, native=True)` for top-level strings. Field names aren't
tagged (they're identifiers, not typed data); `native=True` decodes them as
UTF-8 text, falling back to `bytes` for a field name that isn't valid UTF-8.

### `hset(key, field=None, value=None, mapping=None) -> int`

Sets one field, or multiple fields via `mapping`, and returns the number of
*new* fields added (fields that already existed and were only updated are not
counted). Creates the hash if `key` does not exist.

```python
client.hset("user:1", "name", "ravi")
client.hset("user:1", mapping={"name": "ravi", "age": 30})
```

### `hget(key, field, native=False) -> Any`

Returns a field's value, or `None` if the field or key is missing. With
`native=True`, decodes the value back to its original `str`/`int`/`float`
type instead of returning raw `bytes`.

```python
client.hget("user:1", "name")
client.hget("user:1", "age", native=True)  # 30, not b"30"
```

### `hmset(key, mapping) -> bool`

Sets multiple fields at once. Always returns `True`. Equivalent to
`hset(key, mapping=mapping)` without the added-field count.

```python
client.hmset("user:1", {"name": "ravi", "age": 30})
```

### `hmget(key, fields, native=False) -> list[Any]`

Returns values for the given fields, in the same order, with `None` for any
field that is missing. `native=True` decodes each value as in `hget`.

```python
client.hmget("user:1", ["name", "missing"])
```

### `hdel(key, *fields) -> int`

Removes one or more fields and returns the number actually removed.

```python
client.hdel("user:1", "age", "missing")
```

### `hexists(key, field) -> bool`

Returns whether a field exists on the hash.

```python
client.hexists("user:1", "name")
```

### `hgetall(key, native=False) -> dict[Any, Any]`

Returns all fields and values as a dict. Returns `{}` for a missing key.
`native=True` decodes each value as in `hget`, and decodes field names
(dict keys) as UTF-8 `str` (falling back to `bytes` if a field name isn't
valid UTF-8).

```python
client.hgetall("user:1")
client.hgetall("user:1", native=True)  # {"name": "ravi", "age": 30}
```

### `hkeys(key, native=False) -> list[Any]`

Returns all field names. Returns `[]` for a missing key. `native=True`
decodes each field name as UTF-8 `str` (falling back to `bytes` if it isn't
valid UTF-8).

```python
client.hkeys("user:1")
```

### `hvals(key, native=False) -> list[Any]`

Returns all values. Returns `[]` for a missing key. `native=True` decodes
each value as in `hget`.

```python
client.hvals("user:1")
```

### `hlen(key) -> int`

Returns the number of fields in the hash, or `0` for a missing key.

```python
client.hlen("user:1")
```

### `hincrby(key, field, amount) -> int`

Increments an integer field by `amount` and returns the new integer value.
Creates the hash and/or field (starting from `0`) if either is missing.
Raises `ValueError` if the field's current value is not integer-like.

```python
client.hincrby("user:1", "visits", 1)
```
