# Credish

Credish is a Redis-compatible, in-process cache for Python. It exposes a
`redis-py`-style client backed by a native C extension, so applications can use
familiar Redis commands without running a separate Redis server process.

The project is currently early-stage. Strings, lists, sorted sets, key expiry,
logical databases, and persistence are the main supported areas.

## Features

- In-process cache with a small Python API and native C storage engine
- Redis-like commands for strings, lists, sorted sets, keys, and expiry
- Logical database selection with indexes `0` through `15`
- Persistence modes: `none`, `rdb`, `aof`, and `hybrid`
- Context-manager client for clean native resource handling
- Python 3.10+ packaging with editable installs and wheel build support

## Installation

Credish builds a C extension, so you need Python 3.10 or newer and a C compiler.

For local development:

```bash
python -m venv .venv
source .venv/bin/activate
python -m pip install -e ".[dev]"
```

On Windows PowerShell:

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -e ".[dev]"
```

To rebuild the extension in place:

```bash
python setup.py build_ext --inplace
```

## Quick Start

```python
from credish import CredishClient

with CredishClient(data_dir="./data", persistence="hybrid") as client:
    assert client.ping() == "PONG"

    client.set("name", "credish")
    assert client.get("name") == b"credish"
    assert client.get("name", native=True) == "credish"

    client.incrby("visits", 1)
    assert client.get("visits") == b"1"

    client.rpush("queue", "first", "second")
    assert client.lrange("queue", 0, -1) == [b"first", b"second"]

    client.zadd("leaders", {"ann": 10, "bob": 20})
    assert client.zrange("leaders", 0, -1, withscores=True) == [
        (b"ann", 10.0),
        (b"bob", 20.0),
    ]
```

By default, Redis-style read methods return `bytes`. Use `get(..., native=True)`
to decode values that were stored from supported Python types such as `str`,
`int`, `float`, `bool`, `None`, lists, or dictionaries.

## Client Configuration

```python
from credish import AOF_FSYNC, CredishClient, PERSISTENCE

client = CredishClient(
    data_dir=".",
    persistence=PERSISTENCE.HYBRID,
    save_interval=300,
    aof_fsync=AOF_FSYNC.EVERYSEC,
    db=0,
)
```

| Parameter | Default | Description |
| --- | --- | --- |
| `data_dir` | `"."` | Directory for `credish.rdb` and `credish.aof`. |
| `persistence` | `"hybrid"` | One of `"none"`, `"rdb"`, `"aof"`, or `"hybrid"`. |
| `save_interval` | `300` | Seconds between automatic RDB snapshots where applicable. |
| `aof_fsync` | `"everysec"` | One of `"always"`, `"everysec"`, or `"no"`. |
| `db` | `0` | Initial logical database index, from `0` to `15`. |

Prefer using `CredishClient` as a context manager:

```python
with CredishClient() as client:
    client.set("ready", "yes")
```

## Supported Commands

Implemented command groups include:

- Server: `ping`, `flushdb`, `dbsize`, `select`, `save`, `bgsave`
- Keys and expiry: `delete`, `exists`, `expire`, `pexpire`, `persist`, `ttl`,
  `pttl`, `type`
- Strings: `set`, `get`, `incr`, `incrby`, `decr`, `decrby`
- Lists: `lpush`, `rpush`, `lrange`, `llen`
- Sorted sets: `zadd`, `zrange`, `zrevrange`, `zrank`, `zrevrank`, `zscore`,
  `zrem`, `zcard`, `zrangebyscore`, `zincrby`

Some Python wrapper methods are present ahead of their C extension exports. See
[Errors and Limitations](docs/errors-and-limitations.md) for the current gaps.

## Testing

Run the unit tests with:

```bash
pytest tests/unit
```

Run the full suite, including stress tests:

```bash
pytest
```

## Documentation

- [Getting Started](docs/getting-started.md)
- [API Reference](docs/api-reference.md)
- [Persistence and Expiry](docs/persistence-and-expiry.md)
- [Internals](docs/internals.md)
- [Errors and Limitations](docs/errors-and-limitations.md)
- [Development](docs/development.md)

## License

Credish is licensed under the MIT License.
