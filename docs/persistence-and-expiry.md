# Persistence and Expiry

## Persistence Modes

Credish supports four persistence modes:

| Mode | Description |
| --- | --- |
| `PERSISTENCE.NONE` | Pure in-memory mode. Data is discarded when the client closes. |
| `PERSISTENCE.RDB` | Binary snapshot mode. Use `save()` or `bgsave()` to write snapshots. |
| `PERSISTENCE.AOF` | Append-only file mode. Mutating commands are logged and replayed. |
| `PERSISTENCE.HYBRID` | Combines snapshot and append-only persistence. This is the default. |

Persistence files are created under `data_dir`: a binary snapshot at
`credish.rdb` (used by `rdb`/`hybrid`) and an append-only command log at
`credish.aof` (used by `aof`/`hybrid`).

`save_interval` (default `300` seconds, passed to the `CredishClient`
constructor) is stored on the config but is not currently enforced by any
background timer — there is no autosave loop. RDB snapshots are written only
when you call `save()`/`bgsave()` explicitly, or automatically once when the
store is closed (in `rdb`/`hybrid` mode). Don't rely on it to bound how much
data you could lose in `rdb` mode; call `save()`/`bgsave()` on your own
schedule if you need that guarantee, or use `aof`/`hybrid` mode instead.

### Hybrid Mode Behavior

`hybrid` mode is not a classic "RDB base + AOF delta" load: on startup, a
`hybrid` store replays `credish.aof` only — it does not also load
`credish.rdb`. The RDB snapshot is written on `save()`/`bgsave()` and once
more automatically when the store closes, but it is not consulted when
reopening. In practice this means `hybrid` behaves like `aof` mode for
durability and recovery, with an RDB file kept alongside as a snapshot rather
than as the load source.

## RDB Example

```python
from pathlib import Path
from credish import CredishClient, PERSISTENCE

data_dir = Path("./credish-data")
data_dir.mkdir(exist_ok=True)

with CredishClient(data_dir=str(data_dir), persistence=PERSISTENCE.RDB) as client:
    client.set("rdb_key", "hello")
    client.save()

with CredishClient(data_dir=str(data_dir), persistence=PERSISTENCE.RDB) as client:
    assert client.get("rdb_key") == b"hello"
```

## AOF Example

```python
from credish import CredishClient, PERSISTENCE

with CredishClient(data_dir="./credish-data", persistence=PERSISTENCE.AOF) as client:
    client.set("aof_key", "world")

with CredishClient(data_dir="./credish-data", persistence=PERSISTENCE.AOF) as client:
    assert client.get("aof_key") == b"world"
```

### AOF fsync Modes

`aof_fsync` (default `AOF_FSYNC.EVERYSEC`) controls how aggressively AOF
writes are flushed to disk:

| Mode | Behavior |
| --- | --- |
| `AOF_FSYNC.ALWAYS` | Flush after every appended command. Safest, slowest. |
| `AOF_FSYNC.EVERYSEC` | Intended to flush about once per second in the background. In the current build nothing drives that timer yet, so this behaves like `no` — flushing is left to the OS/libc buffering. |
| `AOF_FSYNC.NO` | Let the OS decide when to flush. |

```python
from credish import CredishClient, PERSISTENCE, AOF_FSYNC

with CredishClient(persistence=PERSISTENCE.AOF, aof_fsync=AOF_FSYNC.ALWAYS) as client:
    client.set("critical", "value")
```

If you need durability guarantees under `everysec`/`no`, call `save()` (RDB
mode) or rely on a clean `close()`, which flushes the AOF file handle.

### What the AOF Actually Logs

Only some mutating commands are currently appended to `credish.aof`: `SET`
(value and encoding, but see below), `INCRBY`, `LPUSH`, `RPUSH`, `ZADD`, and
`ZREM`. `DEL`, `EXPIRE`, `PEXPIRE`, `PERSIST`, and `FLUSHDB` are **not**
appended, even though the AOF replay dispatcher understands their record
format (that code path is currently unreachable, since nothing writes those
records).

In practice, under pure `aof` mode — and under `hybrid` mode, which loads
only from the AOF on startup (see above) — a reload after restart can:

- resurrect keys that were `delete()`d or wiped with `flushdb()` before the
  last write to that key,
- silently drop TTLs, since `expire()`/`pexpire()`/`persist()` are never
  logged and `set(..., ex=...)`/`set(..., px=...)` logs only the value, not
  the expiry.

`save()`/`bgsave()` (RDB) are not affected — they serialize whatever is
currently in memory, including current TTLs and the effect of any deletes or
`flushdb()` calls. If you need expiry or deletions to survive a restart, use
`rdb` mode (or call `save()` explicitly before shutdown in `hybrid` mode)
rather than relying on the AOF replay.

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

Expired keys are treated as missing. Reads perform lazy expiry checks (an
expired key is deleted the moment it's looked up), and a background thread
also actively sweeps each database roughly every 100 ms, sampling expired
keys and deleting them even if they're never read again. See
[Internals: Expiry](internals.md#expiry) for the sampling algorithm.

RDB persists each key's TTL as an absolute millisecond deadline in the
snapshot. AOF does not currently persist TTLs at all — see
[What the AOF Actually Logs](#what-the-aof-actually-logs) above.
