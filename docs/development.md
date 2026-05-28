# Development

## Repository Layout

```text
credish/
  __init__.py
  client.py
  exceptions.py
  _credish.pyi

src/_credish/
  credish_module.c
  db.c / db.h
  object.c / object.h
  sds.c / sds.h
  dict.c / dict.h
  adlist.c / adlist.h
  skiplist.c / skiplist.h
  sorted_set.c / sorted_set.h
  expire.c / expire.h
  persistence/
    rdb.c / rdb.h
    aof.c / aof.h

tests/
  unit/
  stress/

benchmarks/
```

The Python `CredishClient` is intentionally thin. Most behavior should live in
`src/_credish/` and be exported through `credish_module.c`, then wrapped in
`credish/client.py`.

## Testing

Run the test suite with:

```bash
pytest
```

The tests cover:

- string operations and expiry
- list operations
- sorted set operations
- persistence round trips
- common error conditions

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
2. Export it in `credish_module.c`.
3. Add or update the Python wrapper in `credish/client.py`.
4. Update `credish/_credish.pyi`.
5. Add focused tests under `tests/unit/`.
6. Update the relevant docs file.
