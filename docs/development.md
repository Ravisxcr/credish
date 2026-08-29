# Development

## Repository Layout

```text
credish/
  __init__.py
  client.py
  constants.py
  exceptions.py
  _credish.pyi

src/_credish/
  credish_module.c
  platform.h
  server.c / server.h
  db.c / db.h
  object.c / object.h
  sds.c / sds.h
  dict.c / dict.h
  adlist.c / adlist.h
  skiplist.c / skiplist.h
  py_helpers.c / py_helpers.h
  py_string.c / py_string.h
  py_list.c / py_list.h
  py_key.c / py_key.h
  py_hash.c / py_hash.h
  py_zset.c / py_zset.h
  intset.c / intset.h
  bufpool.c / bufpool.h
  expire.c / expire.h
  persistence/
    rdb.c / rdb.h
    aof.c / aof.h

tests/
  unit/
  stress/
```

The Python `CredishClient` is intentionally thin. Most behavior should live in
`src/_credish/` and be exported through domain command files (`py_string.c`,
`py_list.c`, `py_key.c`, `py_hash.c`, `py_zset.c`), registered in `credish_module.c`,
then wrapped in `credish/client.py`. `platform.h`
is the only place OS-specific code (pthreads vs. Windows threading APIs)
should live — new C code should go through its `credish_*` shims rather than
calling `pthread_*` or Win32 APIs directly.

There is no separate top-level `benchmarks/` directory; throughput
benchmarks live in `tests/stress/test_benchmark.py` alongside longer-running
stress tests.

## Testing

Run the fast unit suite with:

```bash
pytest tests/unit
```

or

```bash
task test
```

`task test` runs `pytest tests`, which also picks up `tests/stress/` —
including a benchmark that drives a million SET/GET ops — so it is slower
than running `tests/unit` alone. The packaged wheel's own test step
(`tool.cibuildwheel.test-command` in `pyproject.toml`) likewise only runs
`tests/unit`.

The unit tests cover:

- string operations, encodings, and expiry
- list operations
- sorted set operations
- session isolation across multiple sessions sharing one store
- persistence round trips
- common error conditions

## Taskfile Targets

This project uses [Task](https://taskfile.dev) (`Taskfile.yml`) instead of
`make` so the same commands work unmodified on Windows, macOS, and Linux.
Install it once (`winget install Task.Task`, `brew install go-task`, etc.),
then run `task` or `task --list` for the full list. Every task that runs
Python depends on `venv`, which creates `.venv` on first use.

| Target | Purpose |
| --- | --- |
| `task bootstrap` | Install local packaging tools (`build`, `twine`, `cibuildwheel`) into `.venv`. |
| `task install-dev` | Install this project editable with the `dev` extra (pytest, pytest-timeout, cibuildwheel). |
| `task test` | Run `pytest tests` (unit + stress). |
| `task build` | Clean, then build an sdist and wheel into `dist/`. |
| `task build-wheels` | Clean, then run `cibuildwheel` for the host platform into `dist/`. |
| `task setup-qemu` | Register QEMU emulators for foreign-arch wheel builds (Linux Docker hosts). |
| `task check` | Validate `dist/*` with `twine check`. |
| `task upload` / `task upload-test` | Upload `dist/*` to PyPI / TestPyPI with twine. |
| `task clean` | Remove build artifacts, `dist/`, `*.egg-info`, and compiled extensions. |
| `task build-inplace` | Build the C extension in-place into `credish/` for local development. |

## Building Wheels

Build the configured wheels from the host Python environment:

```bash
task build-wheels
```

By default this builds CPython 3.10 through 3.14 for the host architecture.
Override the build selector via the usual `cibuildwheel` environment
variables (`CIBW_BUILD`, `CIBW_ARCHS_LINUX`, etc.) before invoking the task.
For `aarch64` builds on an `x86_64` Linux host, register QEMU first:

```bash
task setup-qemu
```

## Adding a Command

When adding a new command:

1. Implement the C data operation in the relevant `py_*.c` file (e.g. `py_string.c`, `py_list.c`, `py_hash.c`, `py_zset.c`, `py_key.c`).
2. Export it in `credish_module.c` and register it in the module's `PyMethodDef` table.
3. Add or update the Python wrapper in `credish/client.py`.
4. Update `credish/_credish.pyi`.
5. Add focused tests under `tests/unit/`.
6. Update the relevant docs file.
