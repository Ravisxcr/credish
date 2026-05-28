import json

import pytest
from credish import CredishClient


@pytest.fixture
def client(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="none") as c:
        yield c


def test_set_get(client):
    assert client.set("foo", "bar") is True
    assert client.get("foo") == b"bar"


def test_get_native_json_serializable_values(client):
    cases = {
        "str": "bar",
        "int": 42,
        "float": 3.25,
        "bool": True,
        "none": None,
        "list": [1, "two", False],
        "dict": {"name": "Ada", "ok": True, "items": [1, None]},
    }
    for key, value in cases.items():
        assert client.set(key, value) is True
        if isinstance(value, str):
            expected = value.encode()
        elif isinstance(value, bool) or value is None or isinstance(value, (list, dict)):
            expected = json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode()
        else:
            expected = str(value).encode()
        assert client.get(key) == expected
        assert client.get(key, native=True) == value


def test_get_native_raw_bytes_stay_bytes(client):
    client.set("blob", b"\x00hello")
    assert client.get("blob", native=True) == b"\x00hello"


def test_get_missing(client):
    assert client.get("no_such_key") is None


def test_overwrite(client):
    client.set("k", "v1")
    client.set("k", "v2")
    assert client.get("k") == b"v2"


def test_delete(client):
    client.set("x", "1")
    assert client.delete("x") == 1
    assert client.get("x") is None


def test_exists(client):
    client.set("a", "1")
    assert client.exists("a") == 1
    assert client.exists("z") == 0


def test_nx(client):
    client.set("nx_key", "original")
    client.set("nx_key", "new", nx=True)
    assert client.get("nx_key") == b"original"


def test_xx(client):
    result = client.set("xx_key", "val", xx=True)
    assert result is None   # key did not exist
    client.set("xx_key", "first")
    client.set("xx_key", "second", xx=True)
    assert client.get("xx_key") == b"second"


def test_incr(client):
    client.set("counter", "10")
    assert client.incrby("counter", 5) == 15
    assert client.incrby("counter", -3) == 12


def test_ttl_expire(client):
    client.set("t", "val")
    client.expire("t", 100)
    ttl = client.ttl("t")
    assert 0 < ttl <= 100


def test_persist(client):
    client.set("p", "v")
    client.expire("p", 100)
    client.persist("p")
    assert client.ttl("p") == -1
