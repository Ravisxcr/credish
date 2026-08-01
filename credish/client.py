"""
CredishClient — redis-py-compatible interface backed by the C extension.

All method signatures mirror redis-py so existing code can swap clients
with minimal changes.
"""

from __future__ import annotations
from enum import IntEnum
import json
from typing import Any, Optional, Union
import credish._credish as _credish
from credish.constants import AOF_FSYNC, PERSISTENCE
from credish.exceptions import DataError


class _ValueEncoding(IntEnum):
    RAW = 0
    JSON = 1
    STR = 2
    INT = 3
    FLOAT = 4


def _ensure_json_compatible(value: Any) -> None:
    if value is None or isinstance(value, (str, int, float, bool)):
        return
    if isinstance(value, list):
        for item in value:
            _ensure_json_compatible(item)
        return
    if isinstance(value, dict):
        for key, item in value.items():
            if not isinstance(key, str):
                raise DataError("SET: JSON object keys must be strings")
            _ensure_json_compatible(item)
        return
    raise DataError("SET: value must be bytes or a JSON-compatible Python datatype")


class CredishClient:
    """
    In-process Redis-compatible cache.

    Parameters
    ----------
    data_dir:
        Directory where RDB snapshot and AOF log are stored.
        Defaults to the current working directory.
    persistence:
        ``PERSISTENCE.AOF`` or ``"aof"`` — append-only log.
        ``PERSISTENCE.RDB`` or ``"rdb"`` — periodic binary snapshot.
        ``PERSISTENCE.HYBRID`` or ``"hybrid"`` — RDB base + AOF delta.
        ``PERSISTENCE.NONE`` or ``"none"`` — no persistence.
    save_interval:
        Seconds between automatic RDB snapshots (ignored when persistence="aof").
    aof_fsync:
        ``AOF_FSYNC.ALWAYS`` or ``"always"``
        ``AOF_FSYNC.EVERYSEC`` or ``"everysec"`` (default)
        ``AOF_FSYNC.NO`` or ``"no"``
    db:
        Logical database index (0–15).
    """

    def __init__(
        self,
        data_dir: str = ".",
        persistence: str | PERSISTENCE = PERSISTENCE.HYBRID,
        save_interval: int = 300,
        aof_fsync: str | AOF_FSYNC = AOF_FSYNC.EVERYSEC,
        db: int = 0,
        _store: Any = None,
        _owns_store: bool = True,
    ) -> None:
        self._db_index = self._validate_db(db)
        self._owns_store = _owns_store
        self._closed = False
        if _store is None:
            self._store = _credish.open(
                data_dir=data_dir,
                persistence=persistence,
                save_interval=save_interval,
                aof_fsync=aof_fsync,
                db=db,
            )
        else:
            self._store = _store
        self._refresh_handle()

    @staticmethod
    def _validate_db(db: int) -> int:
        if not isinstance(db, int):
            raise DataError("db must be an integer")
        if db < 0 or db > 15:
            raise ValueError("db index out of range")
        return db

    def _refresh_handle(self) -> None:
        self._db = self._store if self._db_index == 0 else (self._store, self._db_index)

    def close(self) -> None:
        if self._closed:
            return
        if self._owns_store:
            _credish.close(self._store)
        self._closed = True

    def __enter__(self) -> "CredishClient":
        return self

    def __exit__(self, *_: Any) -> None:
        self.close()

    # ------------------------------------------------------------------
    # Server
    # ------------------------------------------------------------------

    def ping(self) -> str:
        return _credish.ping(self._db)

    def flushdb(self) -> bool:
        return _credish.flushdb(self._db)

    def dbsize(self) -> int:
        return _credish.dbsize(self._db)

    def save(self) -> bool:
        """Synchronous RDB snapshot."""
        return _credish.save(self._db)

    def bgsave(self) -> bool:
        """Trigger a background RDB snapshot (non-blocking)."""
        return _credish.bgsave(self._db)

    def select(self, db: int) -> bool:
        db = self._validate_db(db)
        result = _credish.select(self._db, db)
        self._db_index = db
        self._refresh_handle()
        return result

    def session(self, db: Optional[int] = None) -> "CredishClient":
        """Return another client session sharing this store.

        Each session has its own selected logical database, so this mirrors
        opening multiple redis-py clients without creating another Credish
        store in memory.
        """
        return CredishClient(
            db=self._db_index if db is None else db,
            _store=self._store,
            _owns_store=False,
        )

    def client(self, db: Optional[int] = None) -> "CredishClient":
        """redis-py-style alias for creating a shared session."""
        return self.session(db=db)

    # ------------------------------------------------------------------
    # Key / expiry
    # ------------------------------------------------------------------

    def delete(self, *keys: str) -> int:
        return _credish.delete(self._db, list(keys))

    def exists(self, *keys: str) -> int:
        return _credish.exists(self._db, list(keys))

    def expire(self, key: str, seconds: int) -> bool:
        return _credish.expire(self._db, key, seconds)

    def pexpire(self, key: str, milliseconds: int) -> bool:
        return _credish.pexpire(self._db, key, milliseconds)

    def persist(self, key: str) -> bool:
        return _credish.persist(self._db, key)

    def ttl(self, key: str) -> int:
        return _credish.ttl(self._db, key)

    def pttl(self, key: str) -> int:
        return _credish.pttl(self._db, key)

    def type(self, key: str) -> str:
        return _credish.type_(self._db, key)

    def keys(self, pattern: str = "*") -> list[str]:
        return _credish.keys(self._db, pattern)

    # ------------------------------------------------------------------
    # String
    # ------------------------------------------------------------------

    def set(
        self,
        key: str,
        value: Any,
        ex: Optional[int] = None,
        px: Optional[int] = None,
        nx: bool = False,
        xx: bool = False,
    ) -> Optional[bool]:
        if nx and xx:
            raise DataError("SET: nx and xx are mutually exclusive")
        value_encoding = _ValueEncoding.RAW
        stored_value = value
        if isinstance(value, str):
            value_encoding = _ValueEncoding.STR
        elif isinstance(value, bool) or value is None or isinstance(value, (list, dict)):
            _ensure_json_compatible(value)
            stored_value = json.dumps(value, separators=(",", ":"), ensure_ascii=False)
            value_encoding = _ValueEncoding.JSON
        elif isinstance(value, int):
            stored_value = str(value)
            value_encoding = _ValueEncoding.INT
        elif isinstance(value, float):
            stored_value = format(value, ".17g")
            value_encoding = _ValueEncoding.FLOAT
        elif isinstance(value, (bytearray, memoryview)):
            stored_value = bytes(value)
        elif not isinstance(value, bytes):
            raise DataError("SET: value must be bytes or a JSON-compatible Python datatype")
        return _credish.set(self._db, key, stored_value,
                            ex=-1 if ex is None else ex,
                            px=-1 if px is None else px,
                            nx=nx, xx=xx,
                            value_encoding=value_encoding)

    def get(self, key: str, native: bool = False) -> Any:
        value = _credish.get(self._db, key)
        if value is None or not native:
            return value
        encoding = _credish.get_encoding(self._db, key)
        if encoding == _ValueEncoding.JSON:
            return json.loads(value.decode("utf-8"))
        if encoding == _ValueEncoding.STR:
            return value.decode("utf-8")
        if encoding == _ValueEncoding.INT:
            return int(value)
        if encoding == _ValueEncoding.FLOAT:
            return float(value)
        else:
            return value

    def getset(self, key: str, value: Union[str, bytes]) -> Optional[bytes]:
        return _credish.getset(self._db, key, value)

    def setnx(self, key: str, value: Union[str, bytes]) -> bool:
        return _credish.setnx(self._db, key, value)

    def setex(self, key: str, seconds: int, value: Union[str, bytes]) -> bool:
        return _credish.setex(self._db, key, seconds, value)

    def psetex(self, key: str, milliseconds: int, value: Union[str, bytes]) -> bool:
        return _credish.psetex(self._db, key, milliseconds, value)

    def mset(self, mapping: dict[str, Any]) -> bool:
        return _credish.mset(self._db, mapping)

    def mget(self, *keys: str) -> list[Optional[bytes]]:
        return _credish.mget(self._db, list(keys))

    def incr(self, key: str) -> int:
        return _credish.incr(self._db, key)

    def incrby(self, key: str, amount: int) -> int:
        return _credish.incrby(self._db, key, amount)

    def decr(self, key: str) -> int:
        return _credish.decr(self._db, key)

    def decrby(self, key: str, amount: int) -> int:
        return _credish.decrby(self._db, key, amount)

    def append(self, key: str, value: Union[str, bytes]) -> int:
        return _credish.append(self._db, key, value)

    def strlen(self, key: str) -> int:
        return _credish.strlen(self._db, key)

    # ------------------------------------------------------------------
    # List
    # ------------------------------------------------------------------

    def lpush(self, key: str, *values: Any) -> int:
        return _credish.lpush(self._db, key, list(values))

    def rpush(self, key: str, *values: Any) -> int:
        return _credish.rpush(self._db, key, list(values))

    def lpop(self, key: str, count: Optional[int] = None) -> Any:
        return _credish.lpop(self._db, key, count)

    def rpop(self, key: str, count: Optional[int] = None) -> Any:
        return _credish.rpop(self._db, key, count)

    def lrange(self, key: str, start: int, stop: int) -> list[bytes]:
        return _credish.lrange(self._db, key, start, stop)

    def llen(self, key: str) -> int:
        return _credish.llen(self._db, key)

    def lindex(self, key: str, index: int) -> Optional[bytes]:
        return _credish.lindex(self._db, key, index)

    def lset(self, key: str, index: int, value: Any) -> bool:
        return _credish.lset(self._db, key, index, value)

    def lrem(self, key: str, count: int, value: Any) -> int:
        return _credish.lrem(self._db, key, count, value)

    def ltrim(self, key: str, start: int, stop: int) -> bool:
        return _credish.ltrim(self._db, key, start, stop)

    # ------------------------------------------------------------------
    # Hash
    # ------------------------------------------------------------------

    def hset(self, key: str, field: Optional[str] = None, value: Any = None,
             mapping: Optional[dict] = None) -> int:
        if mapping is not None:
            return _credish.hset(self._db, key, mapping=mapping)
        return _credish.hset(self._db, key, field=field, value=value)

    def hget(self, key: str, field: str, native: bool = False) -> Any:
        return _credish.hget(self._db, key, field, native=native)

    def hmset(self, key: str, mapping: dict) -> bool:
        return _credish.hmset(self._db, key, mapping)

    def hmget(self, key: str, fields: list[str], native: bool = False) -> list[Any]:
        return _credish.hmget(self._db, key, fields, native=native)

    def hdel(self, key: str, *fields: str) -> int:
        return _credish.hdel(self._db, key, list(fields))

    def hexists(self, key: str, field: str) -> bool:
        return _credish.hexists(self._db, key, field)

    def hgetall(self, key: str, native: bool = False) -> dict[Any, Any]:
        return _credish.hgetall(self._db, key, native=native)

    def hkeys(self, key: str, native: bool = False) -> list[Any]:
        return _credish.hkeys(self._db, key, native=native)

    def hvals(self, key: str, native: bool = False) -> list[Any]:
        return _credish.hvals(self._db, key, native=native)

    def hlen(self, key: str) -> int:
        return _credish.hlen(self._db, key)

    def hincrby(self, key: str, field: str, amount: int) -> int:
        return _credish.hincrby(self._db, key, field, amount)

    # ------------------------------------------------------------------
    # Set
    # ------------------------------------------------------------------

    def sadd(self, key: str, *members: Any) -> int:
        return _credish.sadd(self._db, key, list(members))

    def srem(self, key: str, *members: Any) -> int:
        return _credish.srem(self._db, key, list(members))

    def smembers(self, key: str) -> set[bytes]:
        return _credish.smembers(self._db, key)

    def sismember(self, key: str, member: Any) -> bool:
        return _credish.sismember(self._db, key, member)

    def scard(self, key: str) -> int:
        return _credish.scard(self._db, key)

    def sunion(self, *keys: str) -> set[bytes]:
        return _credish.sunion(self._db, list(keys))

    def sinter(self, *keys: str) -> set[bytes]:
        return _credish.sinter(self._db, list(keys))

    def sdiff(self, *keys: str) -> set[bytes]:
        return _credish.sdiff(self._db, list(keys))

    # ------------------------------------------------------------------
    # Sorted Set
    # ------------------------------------------------------------------

    def zadd(self, key: str, mapping: dict[str, float],
             nx: bool = False, xx: bool = False, gt: bool = False,
             lt: bool = False, ch: bool = False) -> int:
        return _credish.zadd(self._db, key, mapping, nx=nx, xx=xx,
                             gt=gt, lt=lt, ch=ch)

    def zrange(self, key: str, start: int, stop: int,
               withscores: bool = False) -> list:
        return _credish.zrange(self._db, key, start, stop, withscores=withscores)

    def zrevrange(self, key: str, start: int, stop: int,
                  withscores: bool = False) -> list:
        return _credish.zrevrange(self._db, key, start, stop, withscores=withscores)

    def zrank(self, key: str, member: Any) -> Optional[int]:
        return _credish.zrank(self._db, key, member)

    def zrevrank(self, key: str, member: Any) -> Optional[int]:
        return _credish.zrevrank(self._db, key, member)

    def zscore(self, key: str, member: Any) -> Optional[float]:
        return _credish.zscore(self._db, key, member)

    def zrem(self, key: str, *members: Any) -> int:
        return _credish.zrem(self._db, key, list(members))

    def zcard(self, key: str) -> int:
        return _credish.zcard(self._db, key)

    def zrangebyscore(self, key: str, min: float, max: float,
                      withscores: bool = False) -> list:
        return _credish.zrangebyscore(self._db, key, min, max, withscores=withscores)

    def zincrby(self, key: str, amount: float, member: Any) -> float:
        return _credish.zincrby(self._db, key, amount, member)
