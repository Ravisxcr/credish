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


# Backwards-compatible typo alias for early examples.
PRESISTENCE = PERSISTENCE


__all__ = ["PERSISTENCE", "PRESISTENCE", "AOF_FSYNC"]
