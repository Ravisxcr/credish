#ifndef CREDISH_PY_KEY_H
#define CREDISH_PY_KEY_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>

PyObject *py_delete(PyObject *self, PyObject *args);
PyObject *py_exists(PyObject *self, PyObject *args);
PyObject *py_expire(PyObject *self, PyObject *args);
PyObject *py_pexpire(PyObject *self, PyObject *args);
PyObject *py_persist(PyObject *self, PyObject *args);
PyObject *py_ttl(PyObject *self, PyObject *args);
PyObject *py_pttl(PyObject *self, PyObject *args);
PyObject *py_type(PyObject *self, PyObject *args);

#endif /* CREDISH_PY_KEY_H */

