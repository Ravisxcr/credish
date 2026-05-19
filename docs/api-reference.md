# API Reference

This page documents the command surface currently backed by the C extension.

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

Stores a string value. `value` may be `str`, `bytes`, `int`, or `float`.

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
```

### `get(key: str) -> bytes | None`

Returns a value as `bytes`, or `None` if the key is missing.

```python
client.get("name")
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
