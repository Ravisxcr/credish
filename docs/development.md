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
  sorted_set.c / sorted_set.h
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
`src/_credish/` and be exported through `credish_module.c` (or `sorted_set.c`
for sorted-set commands), then wrapped in `credish/client.py`. `platform.h`
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
make test
```

`make test` runs `pytest tests`, which also picks up `tests/stress/` —
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

## Makefile Targets

Run `make help` for the full list. All targets prefer `.venv/bin/python` if
it exists, falling back to `python` on `PATH`.

| Target | Purpose |
| --- | --- |
| `make bootstrap` | Install local packaging tools (`build`, `twine`, `cibuildwheel`). |
| `make install-dev` | Install this project editable with the `dev` extra (pytest, pytest-timeout, cibuildwheel). |
| `make test` | Run `pytest tests` (unit + stress). |
| `make build` | Clean, then build an sdist and wheel into `dist/`. |
| `make build-wheels` | Clean, then run `cibuildwheel` for the host platform into `dist/`. |
| `make build-linux-wheels-docker` | Build Linux wheels via Docker into `dist/`. |
| `make setup-qemu` | Register QEMU emulators for foreign-arch Docker wheel builds. |
| `make check` | Validate `dist/*` with `twine check`. |
| `make upload` / `make upload-test` | Upload `dist/*` to PyPI / TestPyPI with twine. |
| `make clean` | Remove build artifacts, `dist/`, `*.egg-info`, compiled `.so` files, and `__pycache__`. |

## Building Wheels

Build the configured wheels from the host Python environment:

```bash
make build-wheels
```

Build Linux wheels through Docker and write the wheels to the local `dist/`
directory using mounted source and output folders:

```bash
make build-linux-wheels-docker
```

By default this builds CPython 3.10 through 3.14 for `x86_64`. Override the
build selector or architecture when needed:

```bash
CIBW_BUILD="cp312-* cp313-*" CIBW_ARCHS_LINUX="x86_64 aarch64" make build-linux-wheels-docker
```

For `aarch64` builds on an `x86_64` host, register QEMU first:

```bash
make setup-qemu
```

## Adding a Command

When adding a new command:

1. Implement the C data operation.
2. Export it in `credish_module.c` (or `sorted_set.c` for sorted-set commands)
   and register it in the module's `PyMethodDef` table.
3. Add or update the Python wrapper in `credish/client.py`.
4. Update `credish/_credish.pyi`.
5. Add focused tests under `tests/unit/`.
6. Update the relevant docs file.
