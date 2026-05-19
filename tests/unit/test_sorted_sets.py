import math

import pytest

from credish import CredishClient


@pytest.fixture
def client(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="none") as c:
        yield c


def test_zadd_zrange_and_tie_breaking(client):
    assert client.zadd("leaders", {"bob": 20, "ann": 10, "ada": 10}) == 3
    assert client.type("leaders") == "zset"
    assert client.zrange("leaders", 0, -1) == [b"ada", b"ann", b"bob"]
    assert client.zrange("leaders", 0, -1, withscores=True) == [
        (b"ada", 10.0),
        (b"ann", 10.0),
        (b"bob", 20.0),
    ]
    assert client.zrevrange("leaders", 0, -1) == [b"bob", b"ann", b"ada"]


def test_zadd_updates_rank_and_ch(client):
    client.zadd("leaders", {"ann": 10, "bob": 20})
    assert client.zadd("leaders", {"ann": 30}) == 0
    assert client.zrange("leaders", 0, -1) == [b"bob", b"ann"]
    assert client.zadd("leaders", {"ann": 40}, ch=True) == 1
    assert client.zscore("leaders", "ann") == 40.0


def test_zrank_zrevrank_zscore_and_missing(client):
    client.zadd("leaders", {"a": 1, "b": 2, "c": 3})
    assert client.zrank("leaders", "b") == 1
    assert client.zrevrank("leaders", "b") == 1
    assert client.zrank("leaders", "missing") is None
    assert client.zscore("leaders", "missing") is None


def test_zrem_zcard_zrangebyscore_and_zincrby(client):
    client.zadd("leaders", {"a": 1, "b": 2, "c": 3})
    assert client.zrangebyscore("leaders", 1.5, 3, withscores=True) == [
        (b"b", 2.0),
        (b"c", 3.0),
    ]
    assert client.zrem("leaders", "b", "missing") == 1
    assert client.zcard("leaders") == 2
    assert client.zincrby("leaders", 5, "a") == 6.0
    assert client.zrange("leaders", 0, -1) == [b"c", b"a"]


def test_zadd_options_and_bad_score(client):
    client.zadd("leaders", {"a": 1})
    assert client.zadd("leaders", {"a": 2}, nx=True) == 0
    assert client.zadd("leaders", {"b": 2}, xx=True) == 0
    assert client.zadd("leaders", {"a": 0}, gt=True) == 0
    assert client.zadd("leaders", {"a": 0}, lt=True, ch=True) == 1
    with pytest.raises(ValueError):
        client.zadd("bad", {"x": math.nan})


def test_zset_persistence_roundtrip(tmp_path):
    with CredishClient(data_dir=str(tmp_path), persistence="rdb") as c:
        c.zadd("leaders", {"ann": 10, "bob": 20})
        c.save()

    with CredishClient(data_dir=str(tmp_path), persistence="rdb") as c:
        assert c.zrange("leaders", 0, -1, withscores=True) == [
            (b"ann", 10.0),
            (b"bob", 20.0),
        ]

    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        c.zadd("aofleaders", {"ann": 10, "bob": 20})
        c.zrem("aofleaders", "ann")

    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        assert c.zrange("aofleaders", 0, -1, withscores=True) == [(b"bob", 20.0)]
