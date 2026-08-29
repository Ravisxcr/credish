#include "py_helpers.h"
#include "platform.h"
#include <stdio.h>
#include <string.h>

int g_closed_sentinel;

credish_store *credish_get_store(PyObject *handle) {
    credish_store *store = PyCapsule_GetPointer(handle, CAPSULE_NAME);
    if (!store && PyTuple_Check(handle) && PyTuple_GET_SIZE(handle) == 2) {
        PyErr_Clear();
        store = PyCapsule_GetPointer(PyTuple_GET_ITEM(handle, 0), CAPSULE_NAME);
    }
    if (!store || store == (credish_store *)&g_closed_sentinel) 
        return NULL;
    return store;
}

int get_db_id(PyObject *handle) {
    if (PyTuple_Check(handle) && PyTuple_GET_SIZE(handle) == 2) {
        long db_id = PyLong_AsLong(PyTuple_GET_ITEM(handle, 1));
        if (PyErr_Occurred()) return -1;
        return (int)db_id;
    }
    return 0;
}

credish_db *credish_get_db(PyObject *handle, credish_store *store) {
    if (!PyTuple_Check(handle)) 
        return &store->dbs[0];
    
    int db_id = get_db_id(handle);
    credish_db *db = store_select_db(store, db_id);
    
    if (!db) 
        PyErr_SetString(PyExc_ValueError, "db index out of range");
    return db;
}

/* Decode a Python str/bytes arg to (char*, int) */
int decode_key(PyObject *obj, char **out, int *out_len) {
    if (PyBytes_Check(obj)) {
        *out = PyBytes_AS_STRING(obj);
        *out_len = (int)PyBytes_GET_SIZE(obj);
        return 1;
    }
    if (PyUnicode_Check(obj)) {
        Py_ssize_t sz;
        *out = (char *)PyUnicode_AsUTF8AndSize(obj, &sz);
        *out_len = (int)sz;
        return *out != NULL;
    }
    PyErr_SetString(PyExc_TypeError, "key must be str or bytes");
    return 0;
}

/* Coerce any Python value to an sds (for SET value, LPUSH members, etc.) */
sds pyobj_to_sds(PyObject *object) {
    if (PyBytes_Check(object))
        return sds_newlen(
            PyBytes_AS_STRING(object),
            (size_t)PyBytes_GET_SIZE(object)
        );
    if (PyUnicode_Check(object)) {
        Py_ssize_t utf8_len;
        const char *utf8_str = PyUnicode_AsUTF8AndSize(object, &utf8_len);
        return utf8_str ? sds_newlen(utf8_str, (size_t)utf8_len) : NULL;
    }
    if (PyLong_Check(object)) {
        long long int_value = PyLong_AsLongLong(object);
        char int_buf[24];
        int int_len = snprintf(int_buf, sizeof(int_buf), "%lld", int_value);
        return sds_newlen(int_buf, (size_t)int_len);
    }
    if (PyFloat_Check(object)) {
        double float_value = PyFloat_AS_DOUBLE(object);
        char float_buf[32];
        int float_len = snprintf(float_buf, sizeof(float_buf), "%.17g", float_value);
        return sds_newlen(float_buf, (size_t)float_len);
    }
    PyErr_SetString(PyExc_TypeError, "value must be str, bytes, int, or float");
    return NULL;
}

int64_t now_ms_mod(void) {
    return credish_now_ms();
}

