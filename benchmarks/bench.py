"""
Quick throughput benchmark: SET and GET throughput.
Run:  python benchmarks/bench.py
"""

import time
import tempfile
from credish import CredishClient

N = 1_000_000


def bench(label, fn):
    t0 = time.perf_counter()
    fn()
    elapsed = time.perf_counter() - t0
    print(f"{label:30s}  {N / elapsed:>10.0f} ops/s  ({elapsed*1000:.1f} ms)")


with tempfile.TemporaryDirectory() as tmp:
    with CredishClient(data_dir=tmp, persistence="none") as c:
        def do_set():
            for i in range(N):
                c.set(f"key:{i}", f"value:{i}")

        def do_get():
            for i in range(N):
                c.get(f"key:{i}")

        do_set()   # warm-up
        bench("SET (no persistence)", do_set)
        bench("GET (no persistence)", do_get)
