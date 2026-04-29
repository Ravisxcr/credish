from setuptools import setup, Extension
import os

_credish_sources = [
    "src/_credish/credish_module.c",
    "src/_credish/sds.c",
    "src/_credish/dict.c",
    "src/_credish/adlist.c",
    "src/_credish/skiplist.c",
    "src/_credish/intset.c",
    "src/_credish/object.c",
    "src/_credish/db.c",
    "src/_credish/expire.c",
    "src/_credish/server.c",
    "src/_credish/persistence/rdb.c",
    "src/_credish/persistence/aof.c",
]

_credish_ext = Extension(
    name="credish._credish",
    sources=_credish_sources,
    include_dirs=["src/_credish"],
    extra_compile_args=["-O2", "-Wall", "-Wextra", "-std=c11", "-pthread", "-D_POSIX_C_SOURCE=200112L"],
    extra_link_args=["-pthread"],
)

setup(ext_modules=[_credish_ext])
