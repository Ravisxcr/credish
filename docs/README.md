# Credish Documentation

Credish is a Redis-compatible, in-process cache for Python. It exposes a
`redis-py`-style client backed by a native C extension, so applications can use
familiar Redis commands without running a separate Redis server process.

The current implementation supports strings, lists, sorted sets, key expiry,
logical databases, and persistence. Hashes, sets, and some convenience wrappers
are planned in the Python API but are not all backed by C extension exports yet.

## Documentation Map

- [Getting Started](getting-started.md): installation, build, quick start, client
  configuration, and return types.
- [API Reference](api-reference.md): implemented server, key, string, list, and
  sorted set commands.
- [Persistence and Expiry](persistence-and-expiry.md): persistence modes, RDB,
  AOF, hybrid mode, TTL, and expiry behavior.
- [Errors and Limitations](errors-and-limitations.md): exported exceptions,
  common failures, planned command groups, and compatibility limits.
- [Development](development.md): repository layout, testing, and the workflow for
  adding commands.

## Status

Implemented in the current C extension:

- Server commands: `ping`, `flushdb`, `dbsize`, `select`, `save`, `bgsave`
- Key and expiry commands: `delete`, `exists`, `expire`, `pexpire`, `persist`,
  `ttl`, `pttl`, `type`
- String commands: `set`, `get`, `incrby`
- List commands: `lpush`, `rpush`, `lrange`, `llen`
- Sorted set commands: `zadd`, `zrange`, `zrevrange`, `zrank`, `zrevrank`,
  `zscore`, `zrem`, `zcard`, `zrangebyscore`, `zincrby`

Planned or wrapper-only APIs are called out in
[Errors and Limitations](errors-and-limitations.md).
