import pytest
from credish import CredishClient


def test_rdb_roundtrip(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="rdb") as c:
        c.set("rdb_key", "hello")
        c.save()

    with CredishClient(data_dir=str(tmp_path), persistence="rdb") as c:
        assert c.get("rdb_key") == b"hello"


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


def test_hybrid_roundtrip(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="hybrid") as c:
        c.set("hybrid_key", "data")

    with CredishClient(data_dir=str(tmp_path), persistence="hybrid") as c:
        assert c.get("hybrid_key") == b"data"
