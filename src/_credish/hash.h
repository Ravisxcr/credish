#ifndef CREDISH_HASH_H
#define CREDISH_HASH_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "db.h"

credish_store *credish_get_store(PyObject *handle);
credish_db *credish_get_db(PyObject *handle, credish_store *store);

PyObject *py_hset(PyObject *self, PyObject *args, PyObject *kw);
PyObject *py_hget(PyObject *self, PyObject *args, PyObject *kw);
PyObject *py_hmset(PyObject *self, PyObject *args);
PyObject *py_hmget(PyObject *self, PyObject *args, PyObject *kw);
PyObject *py_hdel(PyObject *self, PyObject *args);
PyObject *py_hexists(PyObject *self, PyObject *args);
PyObject *py_hgetall(PyObject *self, PyObject *args, PyObject *kw);
PyObject *py_hkeys(PyObject *self, PyObject *args, PyObject *kw);
PyObject *py_hvals(PyObject *self, PyObject *args, PyObject *kw);
PyObject *py_hlen(PyObject *self, PyObject *args);
PyObject *py_hincrby(PyObject *self, PyObject *args);

#endif /* CREDISH_HASH_H */
