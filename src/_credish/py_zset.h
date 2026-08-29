#ifndef CREDISH_PY_ZSET_H
#define CREDISH_PY_ZSET_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>

PyObject *py_zadd(PyObject *self, PyObject *args, PyObject *kw);
PyObject *py_zrange(PyObject *self, PyObject *args, PyObject *kw);
PyObject *py_zrevrange(PyObject *self, PyObject *args, PyObject *kw);
PyObject *py_zrank(PyObject *self, PyObject *args);
PyObject *py_zrevrank(PyObject *self, PyObject *args);
PyObject *py_zscore(PyObject *self, PyObject *args);
PyObject *py_zrem(PyObject *self, PyObject *args);
PyObject *py_zcard(PyObject *self, PyObject *args);
PyObject *py_zrangebyscore(PyObject *self, PyObject *args, PyObject *kw);
PyObject *py_zincrby(PyObject *self, PyObject *args);

#endif /* CREDISH_PY_ZSET_H */

