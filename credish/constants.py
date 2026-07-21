from __future__ import annotations

from enum import Enum


class PERSISTENCE(str, Enum):
    NONE = "none"
    RDB = "rdb"
    AOF = "aof"
    HYBRID = "hybrid"


class AOF_FSYNC(str, Enum):
    ALWAYS = "always"
    EVERYSEC = "everysec"
    NO = "no"


__all__ = ["PERSISTENCE", "AOF_FSYNC"]
