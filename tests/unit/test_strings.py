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


def test_set_get_binary_safety_and_scale(client):
    # Binary-safe key and value: embedded NUL bytes and the full byte range.
    binary_key = b"key\x00with\x00nulls"
    binary_value = b"\x00lead" + bytes(range(256)) + b"\x00trail\x00"
    assert client.set(binary_key, binary_value) is True
    assert client.get(binary_key) == binary_value

    # A value larger than the bufpool's biggest slab class (2048 B), forcing
    # the sds allocator to fall back to a raw malloc instead of a pooled slab.
    huge = b"y" * 10_000
    assert client.set("huge", huge) is True
    assert client.get("huge") == huge

    # The client wrapper stringifies ints before crossing into C, so
    # arbitrary-precision Python ints (far outside int64) must round-trip
    # exactly via native decoding, not just as raw bytes.
    big = 10**40
    assert client.set("bignum", big) is True
    assert client.get("bignum") == str(big).encode()
    assert client.get("bignum", native=True) == big

    assert client.set("neg", -12345) is True
    assert client.get("neg", native=True) == -12345


def test_set_binary_safety_survives_aof_roundtrip(tmp_path):
    # The AOF log is a length-prefixed RESP-like format; an embedded NUL
    # byte in a key or value must not truncate the record.
    binary_key = b"k\x00ey"
    binary_value = b"v\x00al" + bytes(range(256))
    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        assert c.set(binary_key, binary_value) is True
        assert c.get(binary_key) == binary_value

    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        assert c.get(binary_key) == binary_value


def test_incrby_binary_key_survives_aof_roundtrip(tmp_path):
    binary_key = b"counter\x00with\x00nulls"
    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        assert c.incrby(binary_key, 5) == 5
        assert c.incrby(binary_key, 3) == 8

    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        assert c.get(binary_key) == b"8"
