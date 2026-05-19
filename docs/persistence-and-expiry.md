# Persistence and Expiry

## Persistence Modes

Credish supports four persistence modes:

| Mode | Description |
| --- | --- |
| `"none"` | Pure in-memory mode. Data is discarded when the client closes. |
| `"rdb"` | Binary snapshot mode. Use `save()` or `bgsave()` to write snapshots. |
| `"aof"` | Append-only file mode. Mutating commands are logged and replayed. |
| `"hybrid"` | Combines snapshot and append-only persistence. This is the default. |

Persistence files are created under `data_dir`.

## RDB Example

```python
from pathlib import Path
from credish import CredishClient

data_dir = Path("./credish-data")
data_dir.mkdir(exist_ok=True)

with CredishClient(data_dir=str(data_dir), persistence="rdb") as client:
    client.set("rdb_key", "hello")
    client.save()

with CredishClient(data_dir=str(data_dir), persistence="rdb") as client:
    assert client.get("rdb_key") == b"hello"
```

## AOF Example

```python
from credish import CredishClient

with CredishClient(data_dir="./credish-data", persistence="aof") as client:
    client.set("aof_key", "world")

with CredishClient(data_dir="./credish-data", persistence="aof") as client:
    assert client.get("aof_key") == b"world"
```

## Expiry and TTL

Expiry can be attached at write time with `set(..., ex=...)` or
`set(..., px=...)`, or after a key exists using `expire()` and `pexpire()`.

```python
client.set("token", "abc", ex=60)
client.ttl("token")

client.pexpire("token", 500)
client.pttl("token")
```

Use `persist()` to remove expiry:

```python
client.persist("token")
assert client.ttl("token") == -1
```

TTL sentinel values:

- `>= 0`: remaining lifetime
- `-1`: key exists but has no expiry
- `-2`: key does not exist

Expired keys are treated as missing. Reads perform lazy expiry checks, and the C
layer also includes active expiry support.
