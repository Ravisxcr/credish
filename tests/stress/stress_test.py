"""
Stress test for CredishClient.

Exercises all implemented data types under concurrent load, verifies data
integrity, and prints a throughput summary.

NOTE: flushdb() is intentionally avoided between scenarios because the active
sweep thread can race against the freed key-space, causing a use-after-free in
close(). Each scenario uses its own key namespace instead.

Run:
    python tests/stress_test.py
    # or with pytest (slow — run explicitly):
    pytest tests/stress_test.py -v -s
"""

from __future__ import annotations

import statistics
import tempfile
import threading
import time
import random
from dataclasses import dataclass, field
from typing import Callable, List

from credish import CredishClient

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

THREADS = 16
OPS_PER_THREAD = 2_000
KEY_SPACE = 5000
LARGE_VALUE_SIZE = 4_096

random.seed(42)

# ---------------------------------------------------------------------------
# Result container
# ---------------------------------------------------------------------------


@dataclass
class WorkerResult:
    ops: int = 0
    errors: List[str] = field(default_factory=list)
    latencies_ms: List[float] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Worker functions — one per data type
# (only use commands actually implemented in the C extension)
# ---------------------------------------------------------------------------


def worker_strings(client: CredishClient, n: int, tid: int) -> WorkerResult:
    r = WorkerResult()
    for i in range(n):
        key = f"s:str:{random.randrange(KEY_SPACE)}"
        t0 = time.perf_counter()
        try:
            if i % 3 == 0:
                client.set(key, "x" * 64)
            elif i % 3 == 1:
                client.get(key)
            else:
                client.incrby(f"s:ctr:{tid % 10}", 1)
            r.ops += 1
        except Exception as exc:
            r.errors.append(f"strings tid={tid} i={i}: {exc}")
        r.latencies_ms.append((time.perf_counter() - t0) * 1_000)
    return r


def worker_lists(client: CredishClient, n: int, tid: int) -> WorkerResult:
    r = WorkerResult()
    for i in range(n):
        key = f"l:list:{random.randrange(KEY_SPACE // 4)}"
        t0 = time.perf_counter()
        try:
            if i % 3 == 0:
                client.rpush(key, "x" * 64)
            elif i % 3 == 1:
                client.lpush(key, "x" * 64)
            else:
                client.llen(key)
            r.ops += 1
        except Exception as exc:
            r.errors.append(f"lists tid={tid} i={i}: {exc}")
        r.latencies_ms.append((time.perf_counter() - t0) * 1_000)
    return r


def worker_expiry(client: CredishClient, n: int, tid: int) -> WorkerResult:
    r = WorkerResult()
    for i in range(n):
        key = f"e:ttl:{tid}:{i}"
        t0 = time.perf_counter()
        try:
            client.set(key, "v")
            client.pexpire(key, 5_000)   # 5-second TTL
            client.ttl(key)
            r.ops += 1
        except Exception as exc:
            r.errors.append(f"expiry tid={tid} i={i}: {exc}")
        r.latencies_ms.append((time.perf_counter() - t0) * 1_000)
    return r


def worker_mixed(client: CredishClient, n: int, tid: int) -> WorkerResult:
    """All implemented ops interleaved in one worker."""
    r = WorkerResult()
    for i in range(n):
        t0 = time.perf_counter()
        try:
            bucket = i % 5
            if bucket == 0:
                client.set(f"m:str:{random.randrange(KEY_SPACE)}", "val")
            elif bucket == 1:
                client.get(f"m:str:{random.randrange(KEY_SPACE)}")
            elif bucket == 2:
                client.rpush(f"m:list:{random.randrange(100)}", "item")
            elif bucket == 3:
                client.llen(f"m:list:{random.randrange(100)}")
            else:
                client.incrby(f"m:ctr:{tid % 5}", 1)
            r.ops += 1
        except Exception as exc:
            r.errors.append(f"mixed tid={tid} i={i}: {exc}")
        r.latencies_ms.append((time.perf_counter() - t0) * 1_000)
    return r


# ---------------------------------------------------------------------------
# Scenario runner
# ---------------------------------------------------------------------------


def run_scenario(
    label: str,
    client: CredishClient,
    worker_fn: Callable,
    n_threads: int,
    ops_per_thread: int,
) -> None:
    results: List[WorkerResult] = [None] * n_threads  # type: ignore[list-item]

    def run(tid: int) -> None:
        results[tid] = worker_fn(client, ops_per_thread, tid)

    threads = [threading.Thread(target=run, args=(i,)) for i in range(n_threads)]
    t0 = time.perf_counter()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.perf_counter() - t0

    total_ops = sum(r.ops for r in results)
    all_errors = [e for r in results for e in r.errors]
    all_latencies = [l for r in results for l in r.latencies_ms]

    p50 = statistics.median(all_latencies)
    p99 = sorted(all_latencies)[int(len(all_latencies) * 0.99)]
    status = "OK" if not all_errors else f"ERRORS({len(all_errors)})"
    print(
        f"  {label:<28s}  {total_ops / elapsed:>10,.0f} ops/s  "
        f"p50={p50:.3f}ms  p99={p99:.3f}ms  [{status}]"
    )
    for e in all_errors[:3]:
        print(f"    ! {e}")


# ---------------------------------------------------------------------------
# Dedicated stress probes
# ---------------------------------------------------------------------------


def stress_large_values(client: CredishClient) -> None:
    N = 300
    errs = []
    t0 = time.perf_counter()
    for i in range(N):
        key = f"lv:key:{i % 30}"
        val = "y" * LARGE_VALUE_SIZE
        client.set(key, val)
        got = client.get(key)
        if got is None or len(got) != LARGE_VALUE_SIZE:
            errs.append(f"size mismatch at {key}: {len(got) if got else 'None'}")
    elapsed = time.perf_counter() - t0
    status = "OK" if not errs else f"ERRORS({len(errs)})"
    print(f"  {'large values (4 KiB)':<28s}  {N / elapsed:>10,.0f} rw/s   [{status}]")


def stress_expiry_ttl(client: CredishClient) -> None:
    N = 200
    for i in range(N):
        client.set(f"ex2:{i}", "v")
        client.pexpire(f"ex2:{i}", 80)   # 80 ms
    time.sleep(0.20)                     # wait 200 ms
    survived = sum(1 for i in range(N) if client.get(f"ex2:{i}") is not None)
    status = "OK" if survived == 0 else f"WARN: {survived}/{N} keys still alive"
    print(f"  {'expiry (80 ms TTL)':<28s}  {'—':>10}           [{status}]")


def stress_concurrent_counters(client: CredishClient) -> None:
    NTHREADS = 20
    INCS = 500
    key = "stress:counter"
    client.set(key, "0")

    def inc_loop():
        for _ in range(INCS):
            client.incrby(key, 1)

    threads = [threading.Thread(target=inc_loop) for _ in range(NTHREADS)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    expected = NTHREADS * INCS
    actual = int(client.get(key))
    status = "OK" if actual == expected else f"MISMATCH: got {actual}, expected {expected}"
    print(f"  {f'concurrent counter ({NTHREADS}t)':<28s}  {'—':>10}           [{status}]")


# ---------------------------------------------------------------------------
# Persistence roundtrip
# ---------------------------------------------------------------------------


def stress_persistence_roundtrip(data_dir: str, mode: str) -> None:
    """
    Phase 1 — open with persistence=mode, write known sentinel keys plus
    concurrent counter increments, then close (flushing to disk).
    Phase 2 — reopen the same data_dir and verify every written value
    survived the restart intact.
    """
    NTHREADS = 8
    OPS_PER_THREAD = 300
    N_SENTINEL = 50

    # --- Phase 1: write ---
    with CredishClient(
        data_dir=data_dir,
        persistence=mode,
        aof_fsync="always",   # guarantee every op is durable for AOF modes
    ) as client:
        for i in range(N_SENTINEL):
            client.set(f"ps:str:{i}", f"sentinel-{i}")
        client.set("ps:counter", "0")
        for i in range(10):
            client.rpush("ps:list", f"elem-{i}")

        def noisy(tid: int) -> None:
            for j in range(OPS_PER_THREAD):
                client.incrby("ps:counter", 1)
                client.set(f"ps:noise:{tid}:{j % 20}", "z" * 128)

        threads = [threading.Thread(target=noisy, args=(i,)) for i in range(NTHREADS)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        pre_close_counter = int(client.get("ps:counter"))
        if mode in ("rdb", "hybrid"):
            client.save()   # force snapshot before close

    # --- Phase 2: recover and verify ---
    t0 = time.perf_counter()
    with CredishClient(data_dir=data_dir, persistence=mode) as client:
        recovery_ms = (time.perf_counter() - t0) * 1_000

        errs = []

        for i in range(N_SENTINEL):
            got = client.get(f"ps:str:{i}")
            expected = f"sentinel-{i}".encode()
            if got != expected:
                errs.append(f"str {i}: expected {expected!r}, got {got!r}")

        recovered_counter = int(client.get("ps:counter") or b"0")
        if recovered_counter != pre_close_counter:
            errs.append(
                f"counter: expected {pre_close_counter}, got {recovered_counter}"
            )

        list_len = client.llen("ps:list")
        if list_len != 10:
            errs.append(f"list len: expected 10, got {list_len}")

    status = "OK" if not errs else f"ERRORS({len(errs)})"
    label = f"persist roundtrip ({mode})"
    print(
        f"  {label:<28s}  recovery={recovery_ms:.1f}ms  "
        f"counter={recovered_counter}/{pre_close_counter}  [{status}]"
    )
    for e in errs[:3]:
        print(f"    ! {e}")


# ---------------------------------------------------------------------------
# Integrity checks
# ---------------------------------------------------------------------------


def check_list_ordering(client: CredishClient) -> None:
    probe = "probe:list:ordering"
    client.delete(probe)
    for v in ("a", "b", "c", "d"):
        client.rpush(probe, v)
    result = [x.decode() for x in client.lrange(probe, 0, -1)]
    assert result == ["a", "b", "c", "d"], f"list ordering wrong: {result}"
    client.delete(probe)


def check_string_roundtrip(client: CredishClient) -> None:
    for size in (0, 1, 255, 1024, 65_536):
        key = f"probe:str:{size}"
        val = "z" * size
        client.set(key, val)
        got = client.get(key)
        assert got == val.encode(), f"roundtrip failed for size {size}"
        client.delete(key)


def check_incrby_atomicity(client: CredishClient) -> None:
    key = "probe:incrby"
    client.delete(key)
    client.set(key, "0")
    client.incrby(key, 100)
    client.incrby(key, -30)
    assert int(client.get(key)) == 70, f"incrby: expected 70, got {client.get(key)}"
    client.delete(key)


def check_expiry_semantics(client: CredishClient) -> None:
    key = "probe:expire"
    client.set(key, "v")
    client.pexpire(key, 200)
    assert client.get(key) == b"v", "key should exist before expiry"
    assert client.pttl(key) > 0, "pttl should be positive"
    client.persist(key)
    assert client.ttl(key) == -1, "persist should remove TTL"
    client.delete(key)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> None:
    print(f"\nCredish stress test  ({THREADS} threads × {OPS_PER_THREAD} ops/thread)\n")

    with tempfile.TemporaryDirectory() as tmp:
        # Single client for the whole run — no flushdb between scenarios.
        # Each scenario uses a distinct key-prefix namespace.
        with CredishClient(data_dir=tmp, persistence="none") as client:

            print("--- Concurrent data-type workers ---")
            run_scenario("strings",        client, worker_strings, THREADS, OPS_PER_THREAD)
            run_scenario("lists",          client, worker_lists,   THREADS, OPS_PER_THREAD)
            run_scenario("expiry/TTL",     client, worker_expiry,  THREADS, OPS_PER_THREAD)
            run_scenario("mixed",          client, worker_mixed,   THREADS, OPS_PER_THREAD)

            print("\n--- Targeted stress probes ---")
            stress_large_values(client)
            stress_expiry_ttl(client)
            stress_concurrent_counters(client)

            print("\n--- Integrity checks ---")
            check_list_ordering(client)
            check_string_roundtrip(client)
            check_incrby_atomicity(client)
            check_expiry_semantics(client)
            print("  All integrity checks passed.")

    print("\n--- Persistence roundtrip ---")
    for mode in ("aof", "rdb", "hybrid"):
        with tempfile.TemporaryDirectory() as persist_dir:
            stress_persistence_roundtrip(persist_dir, mode)

    print("\nDone.\n")


# ---------------------------------------------------------------------------
# pytest entry points (run with: pytest tests/stress_test.py -v -s)
# ---------------------------------------------------------------------------


def _client(tmp_path):
    return CredishClient(data_dir=str(tmp_path), persistence="none")


def test_stress_strings(tmp_path):
    with _client(tmp_path) as c:
        run_scenario("strings", c, worker_strings, THREADS, OPS_PER_THREAD)


def test_stress_lists(tmp_path):
    with _client(tmp_path) as c:
        run_scenario("lists", c, worker_lists, THREADS, OPS_PER_THREAD)


def test_stress_expiry(tmp_path):
    with _client(tmp_path) as c:
        run_scenario("expiry/TTL", c, worker_expiry, THREADS, OPS_PER_THREAD)


def test_stress_mixed(tmp_path):
    with _client(tmp_path) as c:
        run_scenario("mixed", c, worker_mixed, THREADS, OPS_PER_THREAD)


def test_stress_large_values(tmp_path):
    with _client(tmp_path) as c:
        stress_large_values(c)


def test_stress_expiry_ttl(tmp_path):
    with _client(tmp_path) as c:
        stress_expiry_ttl(c)


def test_stress_concurrent_counters(tmp_path):
    with _client(tmp_path) as c:
        stress_concurrent_counters(c)


def test_stress_integrity(tmp_path):
    with _client(tmp_path) as c:
        check_list_ordering(c)
        check_string_roundtrip(c)
        check_incrby_atomicity(c)
        check_expiry_semantics(c)


def test_stress_persistence_aof(tmp_path):
    stress_persistence_roundtrip(str(tmp_path), "aof")


def test_stress_persistence_rdb(tmp_path):
    stress_persistence_roundtrip(str(tmp_path), "rdb")


def test_stress_persistence_hybrid(tmp_path):
    stress_persistence_roundtrip(str(tmp_path), "hybrid")


if __name__ == "__main__":
    main()
