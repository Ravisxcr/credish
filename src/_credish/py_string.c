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

PyObject *py_get(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle;
    PyObject *key_obj;
    PyObject *result;

    if (!PyArg_ParseTuple(args, "OO", &handle, &key_obj)) return NULL;
    credish_store *store = credish_get_store(handle);

    if (!store) return NULL;

    char *key;
    int keylen;

    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    credish_rwlock_wrlock(&store->lock);

    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup(db, key, keylen);

    if (!o) {
        result = Py_None;
        Py_INCREF(result);
    } else if (o->type != OBJ_STRING) {
        credish_rwlock_wrunlock(&store->lock);
        PyErr_SetString(PyExc_TypeError, "WRONGTYPE: not a string");
        return NULL;
    } else {
        int vlen;
        char *vptr = obj_string_ptr(o, &vlen);
        result = PyBytes_FromStringAndSize(vptr, vlen);
    }

    credish_rwlock_wrunlock(&store->lock);

    return result;
}

PyObject *py_get_encoding(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle;
    PyObject *key_obj;
    PyObject *result;

    if (!PyArg_ParseTuple(args, "OO", &handle, &key_obj)) return NULL;
    credish_store *store = credish_get_store(handle);

    if (!store) return NULL;

    char *key;
    int keylen;

    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    credish_rwlock_wrlock(&store->lock);

    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup(db, key, keylen);

    if (!o) {
        result = Py_None;
        Py_INCREF(result);
    } else if (o->type != OBJ_STRING) {
        credish_rwlock_wrunlock(&store->lock);
        PyErr_SetString(PyExc_TypeError, "WRONGTYPE: not a string");
        return NULL;
    } else {
        result = PyLong_FromLong(o->encoding);
    }

    credish_rwlock_wrunlock(&store->lock);

    return result;
}

PyObject *py_set(PyObject *self, PyObject *args, PyObject *kw) {
    (void)self;
    static char *kwlist[] = {"handle","key","value","ex","px","nx","xx","value_encoding",NULL};
    PyObject *handle;
    PyObject *key_obj;
    PyObject *val_obj;
    char *key;
    int keylen;
    int did_set = 0;
    int value_encoding = OBJ_ENCODING_RAW;
    int ex = -1, px = -1, nx = 0, xx = 0;
    char encbuf[12];

    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOO|iibbi", kwlist,
            &handle, &key_obj, &val_obj, &ex, &px, &nx, &xx, &value_encoding))
        return NULL;

    credish_store *store = credish_get_store(handle); if (!store) return NULL;

    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    sds val_sds = pyobj_to_sds(val_obj);

    if (!val_sds) return NULL;

    credishObject *o = obj_steal_string_encoded(val_sds, value_encoding);

    if (!o) {
        sds_free(val_sds);
        return PyErr_NoMemory();
    }

    credish_rwlock_wrlock(&store->lock);

    credish_db *db = credish_get_db(handle, store);

    if (!(nx && db_lookup(db, key, keylen)) &&
        !(xx && !db_lookup(db, key, keylen))) {
        db_set(db, key, keylen, o, store);
        if (ex > 0) db_set_expire(db, key, keylen, now_ms_mod() + (int64_t)ex * 1000LL);
        if (px > 0) db_set_expire(db, key, keylen, now_ms_mod() + (int64_t)px);
        did_set = 1;
    }

    credish_rwlock_wrunlock(&store->lock);

    if (!did_set) {
        obj_free(o);
        Py_RETURN_NONE;
    }

    int enclen = snprintf(encbuf, sizeof(encbuf), "%d", value_encoding);
    const char *argv_arr[] = { key, (char *)o->ptr, "FMT", encbuf };
    size_t argv_lens[] = { (size_t)keylen, (size_t)SDS_LEN((sds)o->ptr), 3, (size_t)enclen };
    aof_append_len(store, "SET", 4, argv_arr, argv_lens);

    Py_RETURN_TRUE;
}

PyObject *py_incrby(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle;
    PyObject *key_obj; 
    long long amount;

    if (!PyArg_ParseTuple(args, "OOL", &handle, &key_obj, &amount)) return NULL;

    credish_store *store = credish_get_store(handle); if (!store) return NULL;

    char *key; 
    int keylen;

    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    credish_rwlock_wrlock(&store->lock);

    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup(db, key, keylen);
    long long val = 0;

    if (o) {
        if (o->type != OBJ_STRING) {
            credish_rwlock_wrunlock(&store->lock);
            PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
            return NULL;
        }

        int vlen; 
        char *vptr = obj_string_ptr(o, &vlen);
        char tmp[64];
        char *end;  

        memcpy(tmp, vptr, vlen < 63 ? (size_t)vlen : 63);
        tmp[vlen < 63 ? vlen : 63] = '\0';

        val = strtoll(tmp, &end, 10);

        if (*end != '\0') {
            credish_rwlock_wrunlock(&store->lock);
            PyErr_SetString(PyExc_ValueError, "not an integer");
            return NULL;
        }
    }

    val += amount;

    char buf[24]; 
    int n = snprintf(buf, sizeof(buf), "%lld", val);

    credishObject *new_o = obj_create_string(buf, n);
    db_set(db, key, keylen, new_o, store);

    credish_rwlock_wrunlock(&store->lock);

    char amount_buf[24];
    int amount_n = snprintf(amount_buf, sizeof(amount_buf), "%lld", amount);
    const char *incrby_argv[] = { key, amount_buf };
    size_t incrby_lens[] = { (size_t)keylen, (size_t)amount_n };
    aof_append_len(store, "INCRBY", 2, incrby_argv, incrby_lens);

    return PyLong_FromLongLong(val);
}

