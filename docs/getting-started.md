# Getting Started

## Installation

Credish builds a native C extension, so you need Python 3.10 or newer and a C
compiler. On Linux and macOS the compiler must support POSIX threads; on
Windows, native threading APIs are used instead.

For local development:

```bash
python -m venv .venv
source .venv/bin/activate
python -m pip install -e ".[dev]"
```

To build the extension in place:

```bash
python setup.py build_ext --inplace
```

## Quick Start

```python
from credish import CredishClient, PERSISTENCE

with CredishClient(data_dir="./data", persistence=PERSISTENCE.HYBRID) as client:
    assert client.ping() == "PONG"

    client.set("name", "credish")
    assert client.get("name") == b"credish"

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

Credish stores string-like values as bytes. Decode returned values when you need
text:

```python
client.set("status", "ready")
status = client.get("status")
print(status.decode("utf-8"))  # ready
```

## Client Configuration

Create a client with `CredishClient`:

```python
from credish import CredishClient, PERSISTENCE, AOF_FSYNC

CredishClient(
    data_dir=".",
    persistence=PERSISTENCE.HYBRID,
    save_interval=300,
    aof_fsync=AOF_FSYNC.EVERYSEC,
    db=0,
)
```

| Parameter | Default | Description |
| --- | --- | --- |
| `data_dir` | `"."` | Directory for persistence files such as `credish.rdb` and `credish.aof`. |
| `persistence` | `PERSISTENCE.HYBRID` | Persistence mode: `PERSISTENCE.NONE`, `PERSISTENCE.RDB`, `PERSISTENCE.AOF`, or `PERSISTENCE.HYBRID`. |
| `save_interval` | `300` | Automatic RDB snapshot interval in seconds where applicable. |
| `aof_fsync` | `AOF_FSYNC.EVERYSEC` | AOF fsync policy: `AOF_FSYNC.ALWAYS`, `AOF_FSYNC.EVERYSEC`, or `AOF_FSYNC.NO`. |
| `db` | `0` | Initial logical database index. Valid indexes are `0` through `15`. |

The client is a context manager. Prefer `with CredishClient(...) as client:` so
native resources and persistence handles are closed cleanly.

## Data Model

Credish supports the following implemented data types:

| Credish type | Redis equivalent | Python read shape |
| --- | --- | --- |
| String | `STRING` | `bytes` or `None` |
| List | `LIST` | `list[bytes]` |
| Sorted set | `ZSET` | `list[bytes]` or `list[tuple[bytes, float]]` |

## Return Types

Credish follows Redis conventions where practical:

- Missing string keys return `None`.
- Read values are returned as `bytes`.
- Integer commands return Python `int`.
- Score commands return Python `float`.
- Mutating commands usually return `bool`, `int`, or `None` depending on Redis
  semantics.
- `ttl()` and `pttl()` return `-2` for missing keys and `-1` for keys without
  expiry.
