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


def test_hset_binary_safety_and_scale(client):
    # Binary-safe field/value: embedded NUL bytes and the full 0..255 byte
    # range must survive intact, since sds/dict.c are length-based, not
    # NUL-terminated-string based.
    binary_field = b"field\x00with\x00nulls\x00" + bytes(range(256))
    binary_value = b"\x00leading-nul" + bytes(range(256)) + b"\x00trailing-nul\x00"
    assert client.hset("h", binary_field, binary_value) == 1
    assert client.hget("h", binary_field) == binary_value

    # Multi-byte UTF-8 (accents + a 4-byte emoji outside the BMP).
    uni_field = "emoji:\U0001F600"
    uni_value = "café-\U0001F4A9"
    assert client.hset("h", uni_field, uni_value) == 1
    assert client.hget("h", uni_field) == uni_value.encode("utf-8")

    # Numeric coercion: negative int, bool (a PyLong subclass in CPython),
    # and float formatted with %.17g.
    assert client.hset("h", "n_int", -42) == 1
    assert client.hget("h", "n_int") == b"-42"
    assert client.hset("h", "n_bool", True) == 1
    assert client.hget("h", "n_bool") == b"1"
    assert client.hset("h", "n_float", 1.5) == 1
    assert client.hget("h", "n_float") == b"1.5"

    # Empty field name and empty value are both legal, distinct entries.
    assert client.hset("h", "", "") == 1
    assert client.hget("h", "") == b""

    # A value larger than the bufpool's biggest slab class (2048 B), forcing
    # the sds allocator to fall back to a raw malloc instead of a pooled slab.
    huge_value = b"x" * 10_000
    assert client.hset("h", "huge", huge_value) == 1
    assert client.hget("h", "huge") == huge_value

    # A large mapping batch forces dict.c to grow/rehash mid-insert.
    big_mapping = {f"field:{i}": f"value:{i}" for i in range(5000)}
    assert client.hset("h", mapping=big_mapping) == 5000

    # Re-applying a mapping that mixes already-existing fields (updates) with
    # one brand-new field: the returned count must reflect only the field
    # that is genuinely new, not the size of the mapping.
    overlapping = dict(big_mapping)
    overlapping["field:0"] = "changed"
    overlapping["brand-new-field"] = "x"
    assert client.hset("h", mapping=overlapping) == 1

    assert client.hlen("h") == 7 + 5000 + 1  # 7 singles + 5000 batch + 1 brand-new
    assert client.hget("h", "field:0") == b"changed"
    assert client.hget("h", "field:4999") == b"value:4999"
    assert client.hget("h", "brand-new-field") == b"x"

    everything = client.hgetall("h")
    assert everything[binary_field] == binary_value
    assert everything[uni_field.encode("utf-8")] == uni_value.encode("utf-8")
    assert sorted(client.hkeys("h")) == sorted(everything.keys())
    assert sorted(client.hvals("h")) == sorted(everything.values())


def test_hset_binary_safety_survives_aof_roundtrip(tmp_path):
    # Same binary-safety guarantee as above, but through a persistence
    # round-trip: the AOF log is a length-prefixed RESP-like format, so an
    # embedded NUL byte in a field/value must not truncate the record.
    binary_field = b"field\x00with\x00nulls"
    binary_value = b"value\x00with\x00embedded\x00nulls-" + bytes(range(256))

    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        assert c.hset("h", binary_field, binary_value) == 1
        assert c.hget("h", binary_field) == binary_value

    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        assert c.hget("h", binary_field) == binary_value


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


def test_hash_native_decoding(client):
    client.hset("h", mapping={"str_field": "hello", "int_field": -42, "float_field": 3.25})
    client.hset("h", "bytes_field", b"\x00raw\x00")
    client.hincrby("h", "counter", 7)

    # Default behaviour is unchanged: always raw bytes.
    assert client.hget("h", "int_field") == b"-42"
    assert client.hgetall("h")["int_field".encode()] == b"-42"

    # native=True decodes each value back to its original Python type.
    assert client.hget("h", "str_field", native=True) == "hello"
    assert client.hget("h", "int_field", native=True) == -42
    assert client.hget("h", "float_field", native=True) == 3.25
    assert client.hget("h", "bytes_field", native=True) == b"\x00raw\x00"
    assert client.hget("h", "counter", native=True) == 7
    assert client.hget("h", "missing", native=True) is None

    # native=True also decodes field names (dict keys) to str.
    assert client.hgetall("h", native=True) == {
        "str_field": "hello",
        "int_field": -42,
        "float_field": 3.25,
        "bytes_field": b"\x00raw\x00",
        "counter": 7,
    }
    assert all(isinstance(k, str) for k in client.hgetall("h", native=True))
    # Default hgetall is untouched: bytes keys, bytes values.
    assert all(isinstance(k, bytes) for k in client.hgetall("h"))

    assert sorted(client.hkeys("h", native=True)) == [
        "bytes_field",
        "counter",
        "float_field",
        "int_field",
        "str_field",
    ]
    assert all(isinstance(k, bytes) for k in client.hkeys("h"))

    assert sorted(client.hvals("h", native=True), key=str) == sorted(
        ["hello", -42, 3.25, b"\x00raw\x00", 7], key=str
    )

    assert client.hmget("h", ["int_field", "str_field", "missing"], native=True) == [
        -42,
        "hello",
        None,
    ]


def test_hash_native_field_name_falls_back_to_bytes_when_not_utf8(client):
    # A field name that isn't valid UTF-8 can't become a str; native=True
    # must fall back to bytes for the key instead of raising/corrupting it.
    invalid_utf8_field = b"\xff\xfe\x80"
    client.hset("h", invalid_utf8_field, "value")
    client.hset("h", "normal", "ok")

    result = client.hgetall("h", native=True)
    # Looking it up by the raw bytes key only succeeds if the key stayed
    # bytes rather than being silently coerced/dropped.
    assert result[invalid_utf8_field] == "value"
    assert result["normal"] == "ok"

    keys = client.hkeys("h", native=True)
    assert invalid_utf8_field in keys
    assert "normal" in keys


def test_hash_native_values_survive_rdb_roundtrip(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="rdb") as c:
        c.hset("h", mapping={"s": "hi", "i": 5, "f": 1.5})
        c.hset("h", "b", b"\x00bin\x00")
        c.save()

    with CredishClient(data_dir=str(tmp_path), persistence="rdb") as c:
        assert c.hgetall("h", native=True) == {
            "s": "hi",
            "i": 5,
            "f": 1.5,
            "b": b"\x00bin\x00",
        }


def test_hash_native_values_survive_aof_roundtrip(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        c.hset("h", mapping={"s": "hi", "i": 5, "f": 1.5})
        c.hset("h", "b", b"\x00bin\x00")
        c.hincrby("h", "i", 2)

    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        assert c.hgetall("h", native=True) == {
            "s": "hi",
            "i": 7,
            "f": 1.5,
            "b": b"\x00bin\x00",
        }


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


def test_hash_decode_responses_server_configuration(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="none", decode_responses=True) as client:
        client.hset("h", mapping={"str_field": "hello", "int_field": -42, "float_field": 3.25})
        client.hset("h", "bytes_field", b"\x00raw\x00")
        client.hincrby("h", "counter", 7)

        # decode_responses=True decodes hash values without passing native=True
        assert client.hget("h", "str_field") == "hello"
        assert client.hget("h", "int_field") == -42
        assert client.hget("h", "float_field") == 3.25
        assert client.hget("h", "bytes_field") == b"\x00raw\x00"
        assert client.hget("h", "counter") == 7
        assert client.hget("h", "missing") is None

        # hmget decodes responses without passing native=True
        assert client.hmget("h", ["int_field", "str_field", "missing"]) == [-42, "hello", None]

        # hgetall decodes fields and values
        assert client.hgetall("h") == {
            "str_field": "hello",
            "int_field": -42,
            "float_field": 3.25,
            "bytes_field": b"\x00raw\x00",
            "counter": 7,
        }

        # hkeys and hvals decode without passing native=True
        assert sorted(client.hkeys("h")) == [
            "bytes_field",
            "counter",
            "float_field",
            "int_field",
            "str_field",
        ]
        assert sorted(client.hvals("h"), key=str) == sorted(
            ["hello", -42, 3.25, b"\x00raw\x00", 7], key=str
        )

        # Explicit native=False overrides decode_responses=True
        assert client.hget("h", "int_field", native=False) == b"-42"
        assert client.hmget("h", ["int_field"], native=False) == [b"-42"]
        assert client.hgetall("h", native=False)["int_field".encode()] == b"-42"
        assert all(isinstance(k, bytes) for k in client.hkeys("h", native=False))
        assert all(isinstance(v, bytes) for v in client.hvals("h", native=False))

        # get() also respects decode_responses=True by default
        client.set("plain_str", "hello_str")
        assert client.get("plain_str") == "hello_str"
        assert client.get("plain_str", native=False) == b"hello_str"


def test_hash_decode_responses_session_propagation(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="none", decode_responses=True) as root:
        sess0 = root.session()
        sess1 = root.client(db=1)

        sess0.hset("h0", "name", "root_user")
        sess1.hset("h1", "name", "session_user")

        assert sess0.hget("h0", "name") == "root_user"
        assert sess1.hget("h1", "name") == "session_user"
        assert sess0.hgetall("h0") == {"name": "root_user"}
        assert sess1.hgetall("h1") == {"name": "session_user"}

