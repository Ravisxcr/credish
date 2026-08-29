#ifndef CREDISH_PY_LIST_H
#define CREDISH_PY_LIST_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>

PyObject *py_lpush(PyObject *self, PyObject *args);
PyObject *py_rpush(PyObject *self, PyObject *args);
PyObject *py_lrange(PyObject *self, PyObject *args);
PyObject *py_llen(PyObject *self, PyObject *args);

#endif /* CREDISH_PY_LIST_H */

