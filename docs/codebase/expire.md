# Active Expiry: `expire.c`

## Overview

[`src/_credish/expire.c`](../../src/_credish/expire.c) runs the background
worker that deletes expired keys without waiting for a command to read them.
Lazy expiry on access is separately implemented in [`db.c`](db.md).

## Sweep Cycle

`expire_sweep_start()` starts one thread per store. Every 100 ms, that thread:

1. Acquires the store write lock.
2. Iterates through all 16 logical databases.
3. Skips databases with no entries in `db->expires`.
4. Collects up to 20 keys whose deadlines have passed.
5. Deletes those keys from both `db->keys` and `db->expires`.
6. Releases the lock.

The expiry dictionary stores absolute Unix-millisecond deadlines, so the
worker computes the current real time and compares each visited entry.

## Key Ownership

The sweep cannot delete entries while its dictionary iterator is active.
Instead, it duplicates each expired SDS key into a small temporary array,
finishes iteration, and then deletes using those copies.

## Lifecycle

| Function | Behavior |
| --- | --- |
| `expire_sweep_start(store)` | Set `sweep_running` and create the worker thread. |
| `expire_sweep_stop(store)` | Clear the flag and join the worker thread. |

`store_open()` and `store_close()` call these functions as part of database
lifecycle management.

## Implementation Notes

The file defines `SWEEP_STOP_RATIO`, but the current sweep algorithm does not
use it. Also, the 20-item cap limits deletions per database per cycle, not the
number of expiration entries examined while looking for expired keys.

