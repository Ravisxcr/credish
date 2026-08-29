#ifndef CREDISH_PY_STRING_H
#define CREDISH_PY_STRING_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>

PyObject *py_get(PyObject *self, PyObject *args);
PyObject *py_get_encoding(PyObject *self, PyObject *args);
PyObject *py_set(PyObject *self, PyObject *args, PyObject *kw);
PyObject *py_incrby(PyObject *self, PyObject *args);

#endif /* CREDISH_PY_STRING_H */
