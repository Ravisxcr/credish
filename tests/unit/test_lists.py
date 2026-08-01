import pytest
from credish import CredishClient


@pytest.fixture
def client(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="none") as c:
        yield c


def test_lpush_lrange(client):
    client.lpush("mylist", "c", "b", "a")
    assert client.lrange("mylist", 0, -1) == [b"a", b"b", b"c"]


def test_rpush(client):
    client.rpush("rl", "x", "y", "z")
    assert client.lrange("rl", 0, -1) == [b"x", b"y", b"z"]


def test_llen(client):
    client.rpush("ll", "a", "b")
    assert client.llen("ll") == 2


def test_lrange_slice(client):
    client.rpush("sl", "0", "1", "2", "3", "4")
    assert client.lrange("sl", 1, 3) == [b"1", b"2", b"3"]


def test_lpush_rpush_binary_safety_and_scale(client):
    # Binary-safe member: embedded NUL bytes and the full 0..255 byte range.
    binary_member = b"\x00lead" + bytes(range(256)) + b"\x00trail\x00"
    client.rpush("bin", binary_member)
    assert client.lrange("bin", 0, -1) == [binary_member]

    # Regression test: pushing plain Python ints/bools used to corrupt the
    # stored value. credish_module.c's pyobj_to_sds() passed the integer's
    # *value* instead of its formatted string length to sds_newlen(), which
    # over-read the stack for positive numbers and, for negative numbers
    # (cast to a ~2^64 size_t "length"), reliably segfaulted the process.
    client.rpush("nums", -5, 0, 42, True)
    assert client.lrange("nums", 0, -1) == [b"-5", b"0", b"42", b"1"]

    # A member larger than the bufpool's biggest slab class (2048 B), forcing
    # the sds allocator to fall back to a raw malloc instead of a pooled slab.
    huge = b"x" * 10_000
    client.rpush("huge", huge)
    assert client.lrange("huge", 0, -1) == [huge]

    # A large batch forces adlist to grow well past any small fixed-size
    # assumption. RPUSH preserves call order; LPUSH reverses it because each
    # call prepends.
    values = [f"v{i}".encode() for i in range(5000)]
    client.rpush("scale_r", *values)
    assert client.llen("scale_r") == 5000
    assert client.lrange("scale_r", 0, -1) == values

    client.lpush("scale_l", *values)
    assert client.lrange("scale_l", 0, -1) == list(reversed(values))


def test_list_binary_safety_survives_aof_roundtrip(tmp_path):
    # The AOF log is a length-prefixed RESP-like format; an embedded NUL
    # byte in a key or member must not truncate the record.
    binary_key = b"list\x00key"
    binary_member = b"member\x00with\x00nulls\x00" + bytes(range(256))

    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        c.rpush(binary_key, binary_member)
        c.lpush(binary_key, b"head\x00value")
        assert c.lrange(binary_key, 0, -1) == [b"head\x00value", binary_member]

    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        assert c.lrange(binary_key, 0, -1) == [b"head\x00value", binary_member]
