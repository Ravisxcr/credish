#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "py_string.h"
#include "py_helpers.h"
#include "db.h"
#include "object.h"
#include "sds.h"
#include "platform.h"
#include "persistence/aof.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

PyObject *py_get(PyObject *self, PyObject *args)
{
    (void)self;
    PyObject *handle;
    PyObject *pyobj_key;
    PyObject *result;

    if (!PyArg_ParseTuple(args, "OO", &handle, &pyobj_key))
        return NULL;
    credish_store *store = credish_get_store(handle);

    if (!store)
        return NULL;

    char *key;
    int keylen;

    if (!decode_key(pyobj_key, &key, &keylen))
        return NULL;

    credish_rwlock_wrlock(&store->lock);

    credish_db *database = credish_get_db(handle, store);
    credishObject *candidate_obj = db_lookup(database, key, keylen);

    if (!candidate_obj)
    {
        result = Py_None;
        Py_INCREF(result);
    }
    else if (candidate_obj->type != OBJ_STRING)
    {
        credish_rwlock_wrunlock(&store->lock);
        PyErr_SetString(PyExc_TypeError, "WRONGTYPE: not a string");
        return NULL;
    }
    else
    {
        int value_len;
        char *value_ptr = obj_string_ptr(candidate_obj, &value_len);
        result = PyBytes_FromStringAndSize(value_ptr, value_len);
    }

    credish_rwlock_wrunlock(&store->lock);

    return result;
}

PyObject *py_get_encoding(PyObject *self, PyObject *args)
{
    (void)self;
    PyObject *handle;
    PyObject *pyobj_key;
    PyObject *result;

    if (!PyArg_ParseTuple(args, "OO", &handle, &pyobj_key))
        return NULL;
    credish_store *store = credish_get_store(handle);

    if (!store)
        return NULL;

    char *key;
    int keylen;

    if (!decode_key(pyobj_key, &key, &keylen))
        return NULL;

    credish_rwlock_wrlock(&store->lock);

    credish_db *database = credish_get_db(handle, store);
    credishObject *candidate_obj = db_lookup(database, key, keylen);

    if (!candidate_obj)
    {
        result = Py_None;
        Py_INCREF(result);
    }
    else if (candidate_obj->type != OBJ_STRING)
    {
        credish_rwlock_wrunlock(&store->lock);
        PyErr_SetString(PyExc_TypeError, "WRONGTYPE: not a string");
        return NULL;
    }
    else
    {
        result = PyLong_FromLong(candidate_obj->encoding);
    }

    credish_rwlock_wrunlock(&store->lock);

    return result;
}

PyObject *py_set(PyObject *self, PyObject *args, PyObject *kwargs)
{
    (void)self;
    static char *kwlist[] = {"handle", "key", "value", "ex", "px", "nx", "xx", "value_encoding", NULL};
    PyObject *handle;
    PyObject *pyobj_key;
    PyObject *pyobj_value;
    char *key;
    int keylen;
    int did_set = 0;
    int value_encoding = OBJ_ENCODING_RAW;
    int ex = -1, px = -1, nx = 0, xx = 0;
    char encbuf[12];

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OOO|iibbi", kwlist,
                                     &handle, &pyobj_key, &pyobj_value, &ex, &px, &nx, &xx, &value_encoding))
        return NULL;

    credish_store *store = credish_get_store(handle);
    if (!store)
        return NULL;

    if (!decode_key(pyobj_key, &key, &keylen))
        return NULL;
    sds value_sds = pyobj_to_sds(pyobj_value);

    if (!value_sds)
        return NULL;

    credishObject *value_obj = obj_steal_string_encoded(value_sds, value_encoding);

    if (!value_obj)
    {
        sds_free(value_sds);
        return PyErr_NoMemory();
    }

    credish_rwlock_wrlock(&store->lock);

    credish_db *database = credish_get_db(handle, store);

    if (!(nx && db_lookup(database, key, keylen)) &&
        !(xx && !db_lookup(database, key, keylen)))
    {
        db_set(database, key, keylen, value_obj, store);
        if (ex > 0)
            db_set_expire(database, key, keylen, now_ms_mod() + (int64_t)ex * 1000LL);
        if (px > 0)
            db_set_expire(database, key, keylen, now_ms_mod() + (int64_t)px);
        did_set = 1;
    }

    credish_rwlock_wrunlock(&store->lock);

    if (!did_set)
    {
        obj_free(value_obj);
        Py_RETURN_NONE;
    }

    int enclen = snprintf(encbuf, sizeof(encbuf), "%d", value_encoding);
    const char *argv_arr[] = {key, (char *)value_obj->ptr, "FMT", encbuf};
    size_t argv_lens[] = {(size_t)keylen, (size_t)SDS_LEN((sds)value_obj->ptr), 3, (size_t)enclen};
    aof_append_len(store, "SET", 4, argv_arr, argv_lens);

    Py_RETURN_TRUE;
}

PyObject *py_incrby(PyObject *self, PyObject *args)
{
    (void)self;
    PyObject *handle;
    PyObject *pyobj_key;
    long long amount;

    if (!PyArg_ParseTuple(args, "OOL", &handle, &pyobj_key, &amount))
        return NULL;

    credish_store *store = credish_get_store(handle);
    if (!store)
        return NULL;

    char *key;
    int keylen;

    if (!decode_key(pyobj_key, &key, &keylen))
        return NULL;

    credish_rwlock_wrlock(&store->lock);

    credish_db *database = credish_get_db(handle, store);
    credishObject *existing_obj = db_lookup(database, key, keylen);
    long long result_val = 0;

    if (existing_obj)
    {
        if (existing_obj->type != OBJ_STRING)
        {
            credish_rwlock_wrunlock(&store->lock);
            PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
            return NULL;
        }

        int val_str_len;
        char *val_str = obj_string_ptr(existing_obj, &val_str_len);
        char num_str_buf[64];
        char *end;

        memcpy(num_str_buf, val_str, val_str_len < 63 ? (size_t)val_str_len : 63);
        num_str_buf[val_str_len < 63 ? val_str_len : 63] = '\0';

        result_val = strtoll(num_str_buf, &end, 10);

        if (*end != '\0')
        {
            credish_rwlock_wrunlock(&store->lock);
            PyErr_SetString(PyExc_ValueError, "not an integer");
            return NULL;
        }
    }

    result_val += amount;

    char result_str_buf[24];
    int result_str_len = snprintf(result_str_buf, sizeof(result_str_buf), "%lld", result_val);

    credishObject *new_obj = obj_create_string(result_str_buf, result_str_len);
    db_set(database, key, keylen, new_obj, store);

    credish_rwlock_wrunlock(&store->lock);

    char amount_buf[24];
    int amount_n = snprintf(amount_buf, sizeof(amount_buf), "%lld", amount);
    const char *incrby_argv[] = {key, amount_buf};
    size_t incrby_lens[] = {(size_t)keylen, (size_t)amount_n};
    aof_append_len(store, "INCRBY", 2, incrby_argv, incrby_lens);

    return PyLong_FromLongLong(result_val);
}
