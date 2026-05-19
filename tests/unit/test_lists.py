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
