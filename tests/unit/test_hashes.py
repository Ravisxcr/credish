import pytest

from credish import CredishClient


@pytest.fixture
def client(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="none") as c:
        yield c


def test_hset_field_value_and_hget(client):
    assert client.hset("user:1", "name", "ravi") == 1
    assert client.type("user:1") == "hash"
    assert client.hget("user:1", "name") == b"ravi"
    assert client.hget("user:1", "missing") is None
    assert client.hget("nokey", "name") is None


def test_hset_returns_count_of_new_fields_only(client):
    assert client.hset("h", "a", "1") == 1
    assert client.hset("h", "a", "2") == 0
    assert client.hget("h", "a") == b"2"


def test_hset_with_mapping(client):
    assert client.hset("h", mapping={"a": "1", "b": "2", "c": "3"}) == 3
    assert client.hset("h", mapping={"a": "10", "d": "4"}) == 1
    assert client.hget("h", "a") == b"10"
    assert client.hget("h", "d") == b"4"


def test_hmset_and_hmget(client):
    assert client.hmset("h", {"a": "1", "b": "2"}) is True
    assert client.hmget("h", ["a", "b", "missing"]) == [b"1", b"2", None]
    assert client.hmget("nokey", ["a"]) == [None]


def test_hdel(client):
    client.hset("h", mapping={"a": "1", "b": "2", "c": "3"})
    assert client.hdel("h", "a", "missing") == 1
    assert client.hexists("h", "a") is False
    assert client.hlen("h") == 2


def test_hexists(client):
    client.hset("h", "a", "1")
    assert client.hexists("h", "a") is True
    assert client.hexists("h", "b") is False
    assert client.hexists("nokey", "a") is False


def test_hgetall_hkeys_hvals(client):
    client.hset("h", mapping={"a": "1", "b": "2"})
    assert client.hgetall("h") == {b"a": b"1", b"b": b"2"}
    assert sorted(client.hkeys("h")) == [b"a", b"b"]
    assert sorted(client.hvals("h")) == [b"1", b"2"]
    assert client.hgetall("nokey") == {}
    assert client.hkeys("nokey") == []
    assert client.hvals("nokey") == []


def test_hlen(client):
    assert client.hlen("nokey") == 0
    client.hset("h", mapping={"a": "1", "b": "2"})
    assert client.hlen("h") == 2


def test_hincrby(client):
    assert client.hincrby("h", "count", 5) == 5
    assert client.hincrby("h", "count", 3) == 8
    assert client.hincrby("h", "count", -10) == -2
    client.hset("h", "other", "hello")
    with pytest.raises(ValueError):
        client.hincrby("h", "other", 1)


def test_hash_wrongtype_errors(client):
    client.set("strkey", "hello")
    with pytest.raises(TypeError, match="WRONGTYPE"):
        client.hset("strkey", "a", "1")
    with pytest.raises(TypeError, match="WRONGTYPE"):
        client.hget("strkey", "a")
    with pytest.raises(TypeError, match="WRONGTYPE"):
        client.hgetall("strkey")

    client.lpush("listkey", "a")
    with pytest.raises(TypeError, match="WRONGTYPE"):
        client.hset("listkey", "a", "1")


def test_hash_persistence_rdb_roundtrip(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="rdb") as c:
        c.hset("h", mapping={"a": "1", "b": "2"})
        c.save()

    with CredishClient(data_dir=str(tmp_path), persistence="rdb") as c:
        assert c.hgetall("h") == {b"a": b"1", b"b": b"2"}


def test_hash_persistence_aof_roundtrip(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        c.hset("h", mapping={"a": "1", "b": "2", "c": "3"})
        c.hdel("h", "b")
        c.hincrby("h", "a", 9)

    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        assert c.hgetall("h") == {b"a": b"10", b"c": b"3"}
