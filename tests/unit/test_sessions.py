from credish import CredishClient


def test_sessions_share_store_with_isolated_databases(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="none") as root:
        db0 = root.session()
        db1 = root.session(db=1)

        db0.set("shared-key", "zero")
        db1.set("shared-key", "one")

        assert root.get("shared-key") == b"zero"
        assert db0.get("shared-key") == b"zero"
        assert db1.get("shared-key") == b"one"
        assert root.dbsize() == 1
        assert db1.dbsize() == 1


def test_select_changes_only_current_session(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="none") as root:
        other = root.session(db=1)

        root.set("k", "db0")
        root.select(1)
        root.set("k", "db1")

        assert root.get("k") == b"db1"
        assert other.get("k") == b"db1"
        other.select(0)
        assert other.get("k") == b"db0"
