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


def test_zadd_binary_safety_and_scale(client):
    # Binary-safe member: embedded NUL bytes and the full 0..255 byte range.
    binary_member = b"\x00lead" + bytes(range(256)) + b"\x00trail\x00"
    assert client.zadd("z", {binary_member: 1.5}) == 1
    assert client.zscore("z", binary_member) == 1.5

    # Extreme/precision-sensitive float scores must round-trip exactly: the
    # score is formatted with "%.17g" into the AOF/RDB and re-parsed with
    # strtod, which is only lossless at full IEEE-754 double precision.
    tricky_scores = {
        "tiny": 5e-300,
        "huge_score": 1.7e308,
        "frac": 0.1,
        "neg": -123456.789,
        "int_like": 42.0,
    }
    assert client.zadd("z", tricky_scores) == len(tricky_scores)
    for member, score in tricky_scores.items():
        assert client.zscore("z", member) == score

    # A large batch forces the skiplist to grow across many levels.
    n = 5000
    batch = {f"m:{i}": float(i) for i in range(n)}
    assert client.zadd("z", batch) == n
    assert client.zcard("z") == n + len(tricky_scores) + 1

    # Re-applying a batch that mixes existing members (updates) with one
    # brand-new member: the returned count must reflect only the new one.
    overlapping = dict(batch)
    overlapping["m:0"] = 999.0
    overlapping["brand-new-member"] = 0.0
    assert client.zadd("z", overlapping) == 1
    assert client.zscore("z", "m:0") == 999.0

    # "neg" is below every other score in the set; "huge_score" is above
    # every other score, regardless of how many other members exist.
    assert client.zrank("z", "neg") == 0
    assert client.zrevrank("z", "huge_score") == 0
    assert client.zscore("z", "m:2500") == 2500.0


def test_zset_binary_safety_survives_aof_roundtrip(tmp_path):
    # The AOF log is a length-prefixed RESP-like format; an embedded NUL
    # byte in a key or member must not truncate the record, for either the
    # ZADD-as-batch or the ZADD-via-ZINCRBY append path.
    binary_member = b"member\x00with\x00nulls\x00" + bytes(range(256))
    doomed_member = b"doomed\x00member"

    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        assert c.zadd("z", {binary_member: 3.5, doomed_member: 1.0}) == 2
        assert c.zincrby("z", 1.5, binary_member) == 5.0
        assert c.zrem("z", doomed_member) == 1

    with CredishClient(data_dir=str(tmp_path), persistence="aof") as c:
        assert c.zscore("z", binary_member) == 5.0
        assert c.zscore("z", doomed_member) is None
