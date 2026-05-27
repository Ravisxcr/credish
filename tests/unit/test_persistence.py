import pytest
from credish import AOF_FSYNC, CredishClient, PERSISTENCE, PRESISTENCE


def test_rdb_roundtrip(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="rdb") as c:
        c.set("rdb_key", "hello")
        c.save()

    with CredishClient(data_dir=str(tmp_path), persistence="rdb") as c:
        assert c.get("rdb_key") == b"hello"


def test_persistence_enum_roundtrip(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence=PERSISTENCE.RDB) as c:
        c.set("enum_key", "hello")
        c.save()

    with CredishClient(data_dir=str(tmp_path), persistence=PRESISTENCE.RDB) as c:
        assert c.get("enum_key") == b"hello"


def test_aof_fsync_enum_roundtrip(tmp_path):
    with CredishClient(
        data_dir=str(tmp_path),
        persistence=PERSISTENCE.AOF,
        aof_fsync=AOF_FSYNC.ALWAYS,
    ) as c:
        c.set("enum_aof_key", "world")

    with CredishClient(
        data_dir=str(tmp_path),
        persistence=PERSISTENCE.AOF,
        aof_fsync=AOF_FSYNC.NO,
    ) as c:
        assert c.get("enum_aof_key") == b"world"


def test_rdb_ignores_and_removes_abandoned_tmp_snapshot(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="rdb") as c:
        c.set("rdb_key", "stable")
        c.save()

    tmp_snapshot = tmp_path / "credish.rdb.tmp"
    tmp_snapshot.write_bytes(b"CREDISH_RDB\n\x01partial")

    with CredishClient(data_dir=str(tmp_path), persistence="rdb") as c:
        assert c.get("rdb_key") == b"stable"

    assert not tmp_snapshot.exists()


def test_aof_roundtrip(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        c.set("aof_key", "world")

    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        assert c.get("aof_key") == b"world"


def test_aof_roundtrip_binary_values_with_null_bytes(tmp_path):
    cases = {
        "leading": b"\x00hello",
        "middle": b"hello\x00world",
        "trailing": b"hello\x00",
        "all_nulls": b"\x00" * 1024,
        "large_binary": (b"header:" + (b"\x00abc123" * 9362))[:64 * 1024],
    }

    with CredishClient(
        data_dir=str(tmp_path),
        persistence=PERSISTENCE.AOF,
        aof_fsync=AOF_FSYNC.ALWAYS,
    ) as c:
        for key, value in cases.items():
            c.set(key, value)
            assert c.get(key) == value

    with CredishClient(data_dir=str(tmp_path), persistence=PERSISTENCE.AOF) as c:
        for key, value in cases.items():
            assert c.get(key) == value


def test_hybrid_roundtrip(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="hybrid") as c:
        c.set("hybrid_key", "data")

    with CredishClient(data_dir=str(tmp_path), persistence="hybrid") as c:
        assert c.get("hybrid_key") == b"data"
