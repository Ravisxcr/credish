# Snapshot Persistence: `rdb.c`

## Overview

[`src/_credish/persistence/rdb.c`](../../../src/_credish/persistence/rdb.c)
implements Credish's binary snapshot format. Snapshots are written to
`data_dir/credish.rdb`, using a temporary file followed by `rename()` on save.

## File Layout

```text
CREDISH_RDB\n
version byte
  repeated non-empty database sections:
    SECTION_DB, db id, key count
      repeated keys:
        object type, key bytes, optional expiry, encoded value
SECTION_EOF
crc32
```

Version `1` supports strings, lists, hashes, sets, and sorted sets. Integers
and lengths are encoded in big-endian byte order.

## Serialization By Type

| Object type | Stored representation |
| --- | --- |
| String | One length-prefixed SDS value. |
| List | Element count followed by ordered SDS elements. |
| Hash | Entry count followed by field/value SDS pairs. |
| Set | Member count followed by SDS members. |
| Sorted set | Member count followed by member SDS and raw double score bytes. |

Each key is stored with its absolute millisecond expiry deadline when one
exists.

## Public Operations

| Function | Behavior |
| --- | --- |
| `rdb_save()` | Iterate all database keyspaces and write a new snapshot. |
| `rdb_load()` | Rebuild objects and TTL entries from an existing snapshot. |
| `rdb_bgsave()` | Start a thread that saves while holding the store read lock. |

Loading constructs values through [`object.c`](../object.md), so normal
ownership rules resume once objects are inserted into the database keyspace.

## Checksum And Concurrency Notes

The writer calculates a CRC32 value over the snapshot payload and appends it to
the file. The current loader validates the magic header and version but does
not read or validate the stored checksum. The writer uses a file-global CRC
accumulator, so overlapping saves would share mutable checksum state.

