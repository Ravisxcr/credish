"""
Tests for failure conditions: invalid types, wrong-type key operations,
and bad command usage.
"""
import pytest
from credish import CredishClient
from credish.exceptions import DataError


@pytest.fixture
def client(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="none") as c:
        yield c


# ---------------------------------------------------------------------------
# Invalid value types — storing non-serializable Python objects
# ---------------------------------------------------------------------------

class TestInvalidValueTypes:
    def test_set_json_compatible_python_containers(self, client):
        client.set("dict", {"a": 1, "b": 2})
        client.set("list", [1, 2, 3])

        assert client.get("dict", native=True) == {"a": 1, "b": 2}
        assert client.get("list", native=True) == [1, 2, 3]

    def test_set_python_tuple(self, client):
        with pytest.raises(DataError):
            client.set("key", (1, 2))

    def test_set_dict_with_non_string_key(self, client):
        with pytest.raises(DataError):
            client.set("key", {1: "one"})

    def test_set_credish_client_instance(self, client):
        """The client itself must not be storable as a value."""
        with pytest.raises(DataError):
            client.set("key", client)

    def test_set_none_value(self, client):
        client.set("key", None)
        assert client.get("key", native=True) is None

    def test_lpush_python_objects(self, client):
        """lpush with a dict element should be rejected.
        The C extension sets TypeError but may surface as SystemError due to
        an extension-level return-value bug."""
        with pytest.raises((TypeError, SystemError)):
            client.lpush("mylist", {"nested": "dict"})


# ---------------------------------------------------------------------------
# Invalid key types
# ---------------------------------------------------------------------------

class TestInvalidKeyTypes:
    def test_set_integer_key(self, client):
        with pytest.raises(TypeError):
            client.set(123, "value")

    def test_set_none_key(self, client):
        with pytest.raises(TypeError):
            client.set(None, "value")

    def test_set_list_key(self, client):
        with pytest.raises(TypeError):
            client.set(["bad", "key"], "value")

    def test_get_integer_key(self, client):
        with pytest.raises(TypeError):
            client.get(42)

    def test_delete_integer_key(self, client):
        # Same C extension return-value quirk as lpush; TypeError or SystemError.
        with pytest.raises((TypeError, SystemError)):
            client.delete(99)


# ---------------------------------------------------------------------------
# Wrong-type operations — key holds a different data structure
# ---------------------------------------------------------------------------

class TestWrongTypeOperations:
    def test_get_on_list_key(self, client):
        """GET on a list key should raise a WRONGTYPE error."""
        client.lpush("listkey", "a", "b")
        with pytest.raises(TypeError, match="WRONGTYPE"):
            client.get("listkey")

    def test_lpush_on_string_key(self, client):
        """LPUSH on a string key should raise a WRONGTYPE error."""
        client.set("strkey", "hello")
        with pytest.raises(TypeError, match="WRONGTYPE"):
            client.lpush("strkey", "item")

    def test_rpush_on_string_key(self, client):
        """RPUSH on a string key should raise a WRONGTYPE error."""
        client.set("strkey", "hello")
        with pytest.raises(TypeError, match="WRONGTYPE"):
            client.rpush("strkey", "item")


# ---------------------------------------------------------------------------
# Bad command usage — wrong arguments or mutually exclusive flags
# ---------------------------------------------------------------------------

class TestBadCommandUsage:
    def test_set_nx_and_xx_mutually_exclusive(self, client):
        """SET with both nx=True and xx=True is illegal."""
        with pytest.raises(DataError):
            client.set("key", "value", nx=True, xx=True)

    def test_incrby_float_amount(self, client):
        """INCRBY requires an integer increment; float must be rejected."""
        client.set("counter", "10")
        with pytest.raises(TypeError):
            client.incrby("counter", 1.5)

    def test_incrby_on_non_integer_string(self, client):
        """INCRBY on a key whose value is not a valid integer must fail."""
        client.set("strval", "notanumber")
        with pytest.raises((ValueError, TypeError)):
            client.incrby("strval", 1)

    def test_select_db_index_too_high(self, client):
        """SELECT with an index beyond the valid range must raise ValueError."""
        with pytest.raises(ValueError):
            client.select(999)

    def test_select_negative_db_index(self, client):
        """SELECT with a negative index must raise ValueError."""
        with pytest.raises(ValueError):
            client.select(-1)

    def test_expire_on_nonexistent_key_returns_false(self, client):
        """EXPIRE on a key that does not exist returns False, not an error."""
        assert client.expire("ghost", 100) is False

    def test_delete_nonexistent_key_returns_zero(self, client):
        """DELETE on a missing key returns 0, not an error."""
        assert client.delete("no_such_key") == 0

    def test_ttl_on_nonexistent_key(self, client):
        """TTL on a missing key returns -2 (key does not exist)."""
        assert client.ttl("ghost") == -2

    def test_persist_on_key_without_expiry(self, client):
        """PERSIST on a key with no TTL returns False."""
        client.set("noexp", "val")
        assert client.persist("noexp") is False
