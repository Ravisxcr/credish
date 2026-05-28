from setuptools import setup, Extension
import os
import sys

_base = os.path.dirname(os.path.abspath(__file__))

_credish_sources = [
    "src/_credish/credish_module.c",
    "src/_credish/bufpool.c",
    "src/_credish/sds.c",
    "src/_credish/dict.c",
    "src/_credish/adlist.c",
    "src/_credish/skiplist.c",
    "src/_credish/intset.c",
    "src/_credish/object.c",
    "src/_credish/sorted_set.c",
    "src/_credish/db.c",
    "src/_credish/expire.c",
    "src/_credish/server.c",
    "src/_credish/persistence/rdb.c",
    "src/_credish/persistence/aof.c",
]

_credish_ext = Extension(
    name="credish._credish",
    sources=_credish_sources,
    include_dirs=[os.path.join(_base, "src", "_credish")],
    extra_compile_args=(
        ["/O2"]
        if sys.platform == "win32"
        else ["-O3", "-Wall", "-Wextra", "-std=c11", "-pthread", "-D_POSIX_C_SOURCE=200112L"]
    ),
    extra_link_args=[] if sys.platform == "win32" else ["-pthread"],
)

setup(ext_modules=[_credish_ext])
