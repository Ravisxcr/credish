# Credish

[![PyPI version](https://img.shields.io/pypi/v/credish.svg)](https://pypi.org/project/credish/)
[![Python versions](https://img.shields.io/pypi/pyversions/credish.svg)](https://pypi.org/project/credish/)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://github.com/Ravisxcr/credish/blob/main/LICENSE)

Credish is a Redis-compatible, in-process cache for Python. It provides a
`redis-py`-style client backed by a native C extension, so applications can use
familiar Redis commands without running a separate Redis server process.

Credish is currently early-stage. The main supported areas are strings, lists,
sorted sets, key expiry, logical databases, and local persistence.

## Why Credish?

- In-process cache with no Redis server process to manage
- Native C storage engine exposed through a small Python API
- Redis-like commands for common strings, lists, sorted sets, keys, and expiry
- Logical database selection with indexes `0` through `15`
- Persistence modes for no persistence, RDB snapshots, AOF logs, or both
- Context-manager client support for clean native resource handling
- Python 3.10+ packaging with editable installs and wheel build support

## Installation

Credish builds a C extension, so you need Python 3.10 or newer and a C compiler.

Install from PyPI:

```bash
python -m pip install credish
```

For local development, install the package in editable mode with development
dependencies:

```bash
cd credish
python -m venv .venv
source .venv/bin/activate
python -m pip install -e ".[dev]"
```

On Windows PowerShell:

```powershell
cd credish
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

Redis-style read methods return `bytes` by default. Use `get(..., native=True)`
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
from credish import CredishClient

with CredishClient() as client:
    client.set("ready", "yes")
```

## Supported Commands

Implemented command groups include:

| Group | Commands |
| --- | --- |
| Server | `ping`, `flushdb`, `dbsize`, `select`, `save`, `bgsave` |
| Keys and expiry | `delete`, `exists`, `expire`, `pexpire`, `persist`, `ttl`, `pttl`, `type` |
| Strings | `set`, `get`, `incr`, `incrby`, `decr`, `decrby` |
| Lists | `lpush`, `rpush`, `lrange`, `llen` |
| Sorted sets | `zadd`, `zrange`, `zrevrange`, `zrank`, `zrevrank`, `zscore`, `zrem`, `zcard`, `zrangebyscore`, `zincrby` |

Some Python wrapper methods are present ahead of their C extension exports. See
[Errors and Limitations](https://github.com/Ravisxcr/credish/blob/main/docs/errors-and-limitations.md) for the current gaps.

## Persistence

Credish can store data in the configured `data_dir` using:

| Mode | Behavior |
| --- | --- |
| `none` | Keep data only in memory. |
| `rdb` | Use point-in-time snapshots. |
| `aof` | Append write operations to an AOF log. |
| `hybrid` | Use both RDB snapshots and AOF logging. |

The generated files are named `credish.rdb` and `credish.aof`.

## Project Status

Credish is not a network Redis server and does not implement the Redis protocol.
It is designed as an embedded cache with Redis-like command semantics for common
workflows.

Current limitations:

- Hash and set command groups are planned but not currently implemented in the C
  extension.
- Several Redis string and list wrappers exist in Python before their C
  extension counterparts.
- Redis compatibility focuses on common command behavior, not complete Redis
  server behavior.

See [Errors and Limitations](https://github.com/Ravisxcr/credish/blob/main/docs/errors-and-limitations.md) for more detail.

## Development

Run the unit tests:

```bash
pytest tests/unit
```

Run the full test suite, including stress tests:

```bash
pytest
```

Build a source distribution and wheel:

```bash
python -m build
```

## Documentation

- [Getting Started](https://github.com/Ravisxcr/credish/blob/main/docs/getting-started.md)
- [API Reference](https://github.com/Ravisxcr/credish/blob/main/docs/api-reference.md)
- [Persistence and Expiry](https://github.com/Ravisxcr/credish/blob/main/docs/persistence-and-expiry.md)
- [Internals](https://github.com/Ravisxcr/credish/blob/main/docs/internals.md)
- [Errors and Limitations](https://github.com/Ravisxcr/credish/blob/main/docs/errors-and-limitations.md)
- [Development](https://github.com/Ravisxcr/credish/blob/main/docs/development.md)

## License

Credish is licensed under the MIT License. See [LICENSE](https://github.com/Ravisxcr/credish/blob/main/LICENSE) for details.

