# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Credish is a Redis-compatible, in-process cache for Python: a `redis-py`-style
client (`credish/`) backed by a native C extension (`src/_credish/`, built as
`credish._credish`). It is a library embedded in the caller's process, not a
network server — there is no RESP protocol server here.

## Commands

Build the C extension in place (required after any change under `src/_credish/`):

```bash
python setup.py build_ext --inplace
# or
task build-inplace
```

Run the fast unit suite (this is what CI / the packaged wheel's test step uses):

```bash
pytest tests/unit
```

Run a single test file or test:

```bash
pytest tests/unit/test_hashes.py
pytest tests/unit/test_hashes.py::test_hincrby
```

Run the full suite, including stress/benchmark tests (slow — includes a
million-op SET/GET benchmark):

```bash
pytest
# or
task test
```

Other Taskfile targets (`task --list` for the full set): `task install-dev`,
`task build` (sdist+wheel to `dist/`), `task build-wheels` (cibuildwheel),
`task clean`. Every Python task depends on `venv`, which creates `.venv` on
first use.

After editing any `.c`/`.h` file under `src/_credish/`, rebuild in place
before running tests — pytest imports the compiled `credish/_credish*.pyd`/`.so`,
not the C sources.

## Architecture

```text
Python application
  -> credish.client.CredishClient          (credish/client.py — thin wrapper)
  -> credish._credish C extension
       - db.c            key space / logical DB layer
       - object.c        credishObject value wrapper + type/encoding tags
       - dict.c, sds.c, adlist.c, skiplist.c, intset.c   data structures
       - hash.c          hash command implementations (py_hset, py_hget, ...)
       - sorted_set.c    sorted-set command implementations (py_zadd, ...)
       - bufpool.c        slab allocator for fixed-size internal structures
       - expire.c         active expiry sweep thread
       - persistence/rdb.c, persistence/aof.c
       - platform.h        OS shims (credish_* prefix) for locks/threads/fsync
       - credish_module.c  Python method table + module init
```

- `_credish.open()` returns a Python capsule wrapping a `credish_store *`
  (`src/_credish/db.h`). `CredishClient` in `credish/client.py` holds that
  handle and forwards every method to a matching `_credish.<name>()` call.
- A `credish_store` owns 16 logical databases (`credish_db`), a
  `credish_config`, a global rwlock, AOF file state, and the active-expiry
  sweep thread. Each `credish_db` is a pair of dicts: `keys` (key -> `credishObject*`)
  and `expires` (key -> absolute ms deadline), kept separate so plain lookups
  stay fast and the expiry sweep only scans keys that actually have a TTL.
- Every stored value is a `credishObject { type, encoding, ptr|ival }`. `type`
  is the Redis-style type tag (`OBJ_STRING`/`LIST`/`SET`/`ZSET`/`HASH`).
  `encoding` only matters for strings: it records which Python type
  (`str`/`int`/`float`/JSON/raw bytes) the value should decode back to, and is
  set by `CredishClient.set()` before it calls into C — this is how
  `get(..., native=True)` round-trips non-bytes Python values. The encoding
  tag is persisted in RDB (v2+) and appended to AOF `SET` records as a
  trailing `FMT <tag>` argument.
- `platform.h` is the *only* place OS-specific primitives should be touched
  (rwlocks, mutexes, threads, monotonic time, fsync) — pthreads on Linux/macOS,
  `SRWLOCK`/`CRITICAL_SECTION`/`_beginthreadex` on Windows, all behind a
  `credish_*` prefix. New C code should call these shims, never `pthread_*` or
  Win32 APIs directly.
- `bufpool.c` is a 12-size-class slab allocator (8B..2048B, 4KB pages) used for
  fixed-size structures (`credishObject`, `dictEntry`, `adlist_node`, SDS
  buffers, ...) to avoid `malloc`/`free` on hot paths. `bufpool_free()` must be
  called with the same size that was passed to `bufpool_alloc()`.
- Expiry is both lazy (checked on lookup in `db_lookup()`) and active (a
  background thread in `expire.c` wakes every 100ms and samples up to 20 keys
  per DB from the `expires` dict, resampling the same DB while >=25% of the
  sample is expired, up to 16 rounds/cycle).
- Persistence has two independent backends under `src/_credish/persistence/`:
  RDB (binary snapshot to `credish.rdb`, written by explicit `save()`/`bgsave()`
  or once on clean close) and AOF (RESP-like command log appended to
  `credish.aof`, replayed on open). **`hybrid` mode loads only from the AOF on
  startup, not from the RDB file** — it behaves like `aof` mode for durability,
  with RDB kept only as an on-demand/on-close snapshot. Only `SET`, `INCRBY`,
  `LPUSH`, `RPUSH`, `ZADD`, and `ZREM` are currently appended to the AOF —
  `DEL`/`EXPIRE`/`PEXPIRE`/`PERSIST`/`FLUSHDB` are not, so a restart under pure
  `aof`/`hybrid` mode can resurrect deleted keys and silently drop TTLs. See
  `docs/persistence-and-expiry.md` for the full implications before relying on
  either mode's durability.
- Locking: a single `credish_rwlock_t` per store guards all DB access (read
  lock for reads, write lock for writes and expiry deletions); `bgsave` runs
  `rdb_save()` on a detached thread under the store's *read* lock rather than
  forking (Credish is embedded in the caller's process, so it can't fork a
  snapshotting child the way Redis does). The buffer pool has independent
  per-slab mutexes, separate from the store lock.

### Adding or changing a command

Touch these in order (see `docs/development.md` and `docs/internals.md`):

1. C data operation + `py_<cmd>` export, registered in the shared
   `credish_methods[]` table in `credish_module.c` (implementations for hash
   and sorted-set commands live in `hash.c`/`sorted_set.c` respectively but
   are declared in their `.h` and registered in the same table).
2. Python wrapper method in `credish/client.py` (`CredishClient` is meant to
   stay thin — behavior belongs in C).
3. Type stub update in `credish/_credish.pyi`.
4. Tests under `tests/unit/` targeting the *Python wrapper*, not the raw
   `_credish` function — the wrapper is the compatibility surface users call.
5. Update the relevant file under `docs/`.

### Current implementation gaps (check before assuming a command works)

- Set commands (`sadd`/`srem`/`smembers`/`sismember`/`scard`/`sunion`/`sinter`/`sdiff`)
  have Python wrappers in `client.py` but no C export yet — calling them
  raises `AttributeError` from `credish._credish`. `intset.c` exists but isn't
  wired into `OBJ_SET` storage or the RDB/AOF formats yet.
- Several string/list/key wrapper methods predate their C export: `getset`,
  `setnx`, `setex`, `psetex`, `mset`, `mget`, `append`, `strlen`, `incr`,
  `decr`, `decrby`, `lpop`, `rpop`, `lindex`, `lset`, `lrem`, `ltrim`, `keys`.
- `AOF_FSYNC.EVERYSEC` isn't wired to a periodic flush timer yet — it
  currently behaves like `AOF_FSYNC.NO`.
- `save_interval` on `CredishClient` is stored but not enforced by any
  autosave loop; RDB snapshots only happen on explicit `save()`/`bgsave()` or
  on clean store close.
- Hash commands (`hset`/`hget`/`hmset`/`hmget`/`hdel`/`hexists`/`hgetall`/
  `hkeys`/`hvals`/`hlen`/`hincrby`) are fully implemented, including RDB and
  AOF persistence — this is newer than the top-level README, which still
  lists hashes as unimplemented; trust `docs/api-reference.md` and
  `docs/errors-and-limitations.md` over the README for current command
  support.

## Testing conventions

Unit tests use a `client` fixture backed by `tmp_path` with
`persistence="none"` unless the test specifically exercises RDB/AOF
persistence (in which case it opens two `CredishClient`s against the same
`tmp_path` sequentially — write, close, reopen, assert). `tests/unit/test_sessions.py`
covers `session()`/`client()` isolation across logical DBs sharing one store.
`tests/stress/` holds longer-running stress and throughput benchmarks and is
excluded from the default wheel test step and from `pytest tests/unit`.
