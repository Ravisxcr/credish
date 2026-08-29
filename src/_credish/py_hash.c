#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "py_hash.h"
#include "py_helpers.h"
#include "object.h"
#include "sds.h"
#include "dict.h"
#include "platform.h"
#include "persistence/aof.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*
 * Hash *values* (never fields/keys) carry a 1-byte type tag ahead of their
 * payload — OBJ_ENCODING_{RAW,STR,INT,FLOAT} — so hget/hmget/hgetall/hvals
 * can hand back the original Python type via native=True. The tag rides
 * inside the opaque sds content, so it persists through RDB/AOF for free.
 */
static sds pyobj_to_tagged_sds(PyObject *obj) {
    int tag;
    const char *payload;
    size_t paylen;
    char numbuf[32];

    if (PyBytes_Check(obj)) {
        tag = OBJ_ENCODING_RAW;
        payload = PyBytes_AS_STRING(obj);
        paylen = (size_t)PyBytes_GET_SIZE(obj);
    } else if (PyUnicode_Check(obj)) {
        Py_ssize_t sz;
        const char *s = PyUnicode_AsUTF8AndSize(obj, &sz);
        if (!s) return NULL;
        tag = OBJ_ENCODING_STR;
        payload = s;
        paylen = (size_t)sz;
    } else if (PyLong_Check(obj)) {
        long long v = PyLong_AsLongLong(obj);
        paylen = (size_t)snprintf(numbuf, sizeof(numbuf), "%lld", v);
        tag = OBJ_ENCODING_INT;
        payload = numbuf;
    } else if (PyFloat_Check(obj)) {
        double v = PyFloat_AS_DOUBLE(obj);
        paylen = (size_t)snprintf(numbuf, sizeof(numbuf), "%.17g", v);
        tag = OBJ_ENCODING_FLOAT;
        payload = numbuf;
    } else {
        PyErr_SetString(PyExc_TypeError, "value must be str, bytes, int, or float");
        return NULL;
    }

    sds out = sds_newlen(NULL, paylen + 1);
    if (!out) return NULL;
    out[0] = (char)tag;
    if (paylen) memcpy(out + 1, payload, paylen);
    return out;
}

/* Strip the leading tag byte and return the raw payload as bytes. */
static PyObject *tagged_value_to_bytes(sds val) {
    size_t len = SDS_LEN(val);
    size_t paylen = len > 0 ? len - 1 : 0;
    return PyBytes_FromStringAndSize(val + 1, (Py_ssize_t)paylen);
}

/* Strip the leading tag byte and decode according to it. Values written by
 * this module always carry a recognised tag; anything else falls back to
 * bytes, matching the top-level GET(native=True) RAW behaviour. */
static PyObject *tagged_value_to_native(sds val) {
    size_t len = SDS_LEN(val);
    int tag = (unsigned char)val[0];
    const char *payload = val + 1;
    Py_ssize_t paylen = (Py_ssize_t)(len > 0 ? len - 1 : 0);

    switch (tag) {
        case OBJ_ENCODING_STR:
            return PyUnicode_FromStringAndSize(payload, paylen);
        case OBJ_ENCODING_INT:
            return PyLong_FromString(payload, NULL, 10);
        case OBJ_ENCODING_FLOAT:
            return PyFloat_FromDouble(strtod(payload, NULL));
        default:
            return PyBytes_FromStringAndSize(payload, paylen);
    }
}

/* Field names are never tagged (they're identifiers, not typed data), so
 * native=True just means "decode as UTF-8 text like a normal dict key" —
 * falling back to bytes for a field name that isn't valid UTF-8. */
static PyObject *field_key_to_py(const char *data, Py_ssize_t len, int native) {
    if (!native) return PyBytes_FromStringAndSize(data, len);
    PyObject *s = PyUnicode_DecodeUTF8(data, len, NULL);
    if (s) return s;
    PyErr_Clear();
    return PyBytes_FromStringAndSize(data, len);
}

static void free_sds_array(sds *arr, Py_ssize_t n) {
    if (!arr) return;
    for (Py_ssize_t i = 0; i < n; i++) sds_free(arr[i]);
}

/* Build parallel field/value sds arrays from a Python dict {field: value}. */
static int mapping_to_sds_pairs(PyObject *mapping, sds **fields_out, sds **values_out, Py_ssize_t *n_out) {
    Py_ssize_t n = PyDict_Size(mapping);
    *n_out = n;
    if (n == 0) { *fields_out = NULL; *values_out = NULL; return 0; }

    sds *fields = calloc((size_t)n, sizeof(sds));
    sds *values = calloc((size_t)n, sizeof(sds));
    if (!fields || !values) { free(fields); free(values); PyErr_NoMemory(); return -1; }

    PyObject *mk, *mv;
    Py_ssize_t pos = 0, count = 0;
    while (PyDict_Next(mapping, &pos, &mk, &mv)) {
        sds f = pyobj_to_sds(mk);
        sds v = f ? pyobj_to_tagged_sds(mv) : NULL;
        if (!f || !v) {
            if (f) sds_free(f);
            if (v) sds_free(v);
            free_sds_array(fields, count);
            free_sds_array(values, count);
            free(fields); free(values);
            return -1;
        }
        fields[count] = f;
        values[count] = v;
        count++;
    }

    *fields_out = fields;
    *values_out = values;
    return 0;
}

/* Store field/value pairs into the hash at key (creating it if needed),
 * emit a single HSET AOF record, and report how many fields were new. */
static int hash_apply_fields(credish_store *store, PyObject *handle, char *key, int keylen,
                              sds *fields, sds *values, Py_ssize_t n, int *added_out) {
    credish_rwlock_wrlock(&store->lock);
    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup_write(db, key, keylen);

    if (!o) {
        o = obj_create_hash();
        if (o) db_set(db, key, keylen, o, store);
    } else if (o->type != OBJ_HASH) {
        credish_rwlock_wrunlock(&store->lock);
        PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
        return -1;
    }
    if (!o) {
        credish_rwlock_wrunlock(&store->lock);
        PyErr_NoMemory();
        return -1;
    }

    dict *d = (dict *)o->ptr;
    int added = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        int is_new = dict_find(d, fields[i]) == NULL;
        dict_replace(d, fields[i], values[i]);
        added += is_new;
    }
    credish_rwlock_wrunlock(&store->lock);

    const char **aof_argv = malloc((size_t)(1 + 2 * n) * sizeof(char *));
    size_t *aof_lens = malloc((size_t)(1 + 2 * n) * sizeof(size_t));
    if (aof_argv && aof_lens) {
        aof_argv[0] = key; aof_lens[0] = (size_t)keylen;
        for (Py_ssize_t i = 0; i < n; i++) {
            aof_argv[1 + 2 * i] = fields[i]; aof_lens[1 + 2 * i] = SDS_LEN(fields[i]);
            aof_argv[2 + 2 * i] = values[i]; aof_lens[2 + 2 * i] = SDS_LEN(values[i]);
        }
        aof_append_len(store, "HSET", (int)(1 + 2 * n), aof_argv, aof_lens);
    }
    free(aof_argv);
    free(aof_lens);

    if (added_out) *added_out = added;
    return 0;
}

PyObject *py_hset(PyObject *self, PyObject *args, PyObject *kw) {
    (void)self;
    static char *kwlist[] = {"handle","key","field","value","mapping",NULL};
    PyObject *handle, *key_obj;
    PyObject *field_obj = NULL, *value_obj = NULL, *mapping = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|OOO", kwlist,
            &handle, &key_obj, &field_obj, &value_obj, &mapping))
        return NULL;

    int has_mapping = mapping && mapping != Py_None;
    if (has_mapping && !PyDict_Check(mapping)) {
        PyErr_SetString(PyExc_TypeError, "mapping must be a dict");
        return NULL;
    }
    if (!has_mapping && (!field_obj || field_obj == Py_None || !value_obj)) {
        PyErr_SetString(PyExc_ValueError, "HSET requires either field/value or mapping");
        return NULL;
    }

    credish_store *store = credish_get_store(handle); if (!store) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    sds *fields, *values;
    Py_ssize_t n;

    if (has_mapping) {
        if (mapping_to_sds_pairs(mapping, &fields, &values, &n) != 0) return NULL;
        if (n == 0) return PyLong_FromLong(0);
    } else {
        fields = calloc(1, sizeof(sds));
        values = calloc(1, sizeof(sds));
        if (!fields || !values) { free(fields); free(values); return PyErr_NoMemory(); }
        sds f = pyobj_to_sds(field_obj);
        sds v = f ? pyobj_to_tagged_sds(value_obj) : NULL;
        if (!f || !v) {
            if (f) sds_free(f);
            if (v) sds_free(v);
            free(fields); free(values);
            return NULL;
        }
        fields[0] = f; values[0] = v;
        n = 1;
    }

    int added = 0;
    int rc = hash_apply_fields(store, handle, key, keylen, fields, values, n, &added);

    free_sds_array(fields, n);
    free_sds_array(values, n);
    free(fields); free(values);

    if (rc != 0) return NULL;
    return PyLong_FromLong(added);
}

PyObject *py_hget(PyObject *self, PyObject *args, PyObject *kw) {
    (void)self;
    static char *kwlist[] = {"handle","key","field","native",NULL};
    PyObject *handle, *key_obj, *field_obj;
    int native = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOO|p", kwlist,
            &handle, &key_obj, &field_obj, &native))
        return NULL;
    credish_store *store = credish_get_store(handle); if (!store) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    sds field = pyobj_to_sds(field_obj);
    if (!field) return NULL;

    credish_rwlock_wrlock(&store->lock);
    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup(db, key, keylen);
    PyObject *result = Py_None;
    Py_INCREF(result);
    if (o) {
        if (o->type != OBJ_HASH) {
            credish_rwlock_wrunlock(&store->lock);
            sds_free(field);
            Py_DECREF(result);
            PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
            return NULL;
        }
        sds val = dict_fetch_value((dict *)o->ptr, field);
        if (val) {
            Py_DECREF(result);
            result = native ? tagged_value_to_native(val) : tagged_value_to_bytes(val);
        }
    }
    credish_rwlock_wrunlock(&store->lock);
    sds_free(field);
    return result;
}

PyObject *py_hmset(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj, *mapping;
    if (!PyArg_ParseTuple(args, "OOO", &handle, &key_obj, &mapping)) return NULL;
    if (!PyDict_Check(mapping)) {
        PyErr_SetString(PyExc_TypeError, "mapping must be a dict");
        return NULL;
    }
    credish_store *store = credish_get_store(handle); if (!store) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    sds *fields, *values;
    Py_ssize_t n;
    if (mapping_to_sds_pairs(mapping, &fields, &values, &n) != 0) return NULL;
    if (n == 0) Py_RETURN_TRUE;

    int rc = hash_apply_fields(store, handle, key, keylen, fields, values, n, NULL);

    free_sds_array(fields, n);
    free_sds_array(values, n);
    free(fields); free(values);

    if (rc != 0) return NULL;
    Py_RETURN_TRUE;
}

PyObject *py_hmget(PyObject *self, PyObject *args, PyObject *kw) {
    (void)self;
    static char *kwlist[] = {"handle","key","fields","native",NULL};
    PyObject *handle, *key_obj, *fields_list;
    int native = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOO|p", kwlist,
            &handle, &key_obj, &fields_list, &native))
        return NULL;
    if (!PyList_Check(fields_list)) { PyErr_SetString(PyExc_TypeError, "expected list"); return NULL; }
    credish_store *store = credish_get_store(handle); if (!store) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    Py_ssize_t n = PyList_GET_SIZE(fields_list);
    sds *fields = n ? calloc((size_t)n, sizeof(sds)) : NULL;
    if (n && !fields) return PyErr_NoMemory();
    for (Py_ssize_t i = 0; i < n; i++) {
        fields[i] = pyobj_to_sds(PyList_GET_ITEM(fields_list, i));
        if (!fields[i]) {
            free_sds_array(fields, i);
            free(fields);
            return NULL;
        }
    }

    credish_rwlock_wrlock(&store->lock);
    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup(db, key, keylen);
    if (o && o->type != OBJ_HASH) {
        credish_rwlock_wrunlock(&store->lock);
        free_sds_array(fields, n); free(fields);
        PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
        return NULL;
    }
    dict *d = o ? (dict *)o->ptr : NULL;
    PyObject *result = PyList_New(n);
    if (!result) {
        credish_rwlock_wrunlock(&store->lock);
        free_sds_array(fields, n); free(fields);
        return NULL;
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        sds val = d ? dict_fetch_value(d, fields[i]) : NULL;
        PyObject *item;
        if (val) {
            item = native ? tagged_value_to_native(val) : tagged_value_to_bytes(val);
        } else {
            item = Py_None;
            Py_INCREF(item);
        }
        PyList_SET_ITEM(result, i, item);
    }
    credish_rwlock_wrunlock(&store->lock);

    free_sds_array(fields, n); free(fields);
    return result;
}

PyObject *py_hdel(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj, *fields_list;
    if (!PyArg_ParseTuple(args, "OOO", &handle, &key_obj, &fields_list)) return NULL;
    if (!PyList_Check(fields_list)) { PyErr_SetString(PyExc_TypeError, "expected list"); return NULL; }
    credish_store *store = credish_get_store(handle); if (!store) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    Py_ssize_t n = PyList_GET_SIZE(fields_list);
    sds *fields = n ? calloc((size_t)n, sizeof(sds)) : NULL;
    if (n && !fields) return PyErr_NoMemory();
    for (Py_ssize_t i = 0; i < n; i++) {
        fields[i] = pyobj_to_sds(PyList_GET_ITEM(fields_list, i));
        if (!fields[i]) {
            free_sds_array(fields, i);
            free(fields);
            return NULL;
        }
    }

    sds *removed_fields = n ? calloc((size_t)n, sizeof(sds)) : NULL;
    Py_ssize_t removed_count = 0;
    int removed = 0;

    credish_rwlock_wrlock(&store->lock);
    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup_write(db, key, keylen);
    if (o) {
        if (o->type != OBJ_HASH) {
            credish_rwlock_wrunlock(&store->lock);
            free_sds_array(fields, n); free(fields);
            free(removed_fields);
            PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
            return NULL;
        }
        dict *d = (dict *)o->ptr;
        for (Py_ssize_t i = 0; i < n; i++) {
            if (dict_delete(d, fields[i]) == 0) {
                removed++;
                removed_fields[removed_count++] = fields[i];
            }
        }
    }
    credish_rwlock_wrunlock(&store->lock);

    if (removed_count > 0) {
        const char **aof_argv = malloc((size_t)(1 + removed_count) * sizeof(char *));
        size_t *aof_lens = malloc((size_t)(1 + removed_count) * sizeof(size_t));
        if (aof_argv && aof_lens) {
            aof_argv[0] = key; aof_lens[0] = (size_t)keylen;
            for (Py_ssize_t i = 0; i < removed_count; i++) {
                aof_argv[1 + i] = removed_fields[i];
                aof_lens[1 + i] = SDS_LEN(removed_fields[i]);
            }
            aof_append_len(store, "HDEL", (int)(1 + removed_count), aof_argv, aof_lens);
        }
        free(aof_argv);
        free(aof_lens);
    }

    free_sds_array(fields, n); free(fields);
    free(removed_fields);
    return PyLong_FromLong(removed);
}

PyObject *py_hexists(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj, *field_obj;
    if (!PyArg_ParseTuple(args, "OOO", &handle, &key_obj, &field_obj)) return NULL;
    credish_store *store = credish_get_store(handle); if (!store) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    sds field = pyobj_to_sds(field_obj);
    if (!field) return NULL;

    credish_rwlock_wrlock(&store->lock);
    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup(db, key, keylen);
    int exists = 0;
    if (o) {
        if (o->type != OBJ_HASH) {
            credish_rwlock_wrunlock(&store->lock);
            sds_free(field);
            PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
            return NULL;
        }
        exists = dict_find((dict *)o->ptr, field) != NULL;
    }
    credish_rwlock_wrunlock(&store->lock);
    sds_free(field);

    if (exists) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

PyObject *py_hgetall(PyObject *self, PyObject *args, PyObject *kw) {
    (void)self;
    static char *kwlist[] = {"handle","key","native",NULL};
    PyObject *handle, *key_obj;
    int native = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|p", kwlist, &handle, &key_obj, &native))
        return NULL;
    credish_store *store = credish_get_store(handle); if (!store) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    credish_rwlock_wrlock(&store->lock);
    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup(db, key, keylen);
    PyObject *result = PyDict_New();
    if (!result) { credish_rwlock_wrunlock(&store->lock); return NULL; }
    if (o) {
        if (o->type != OBJ_HASH) {
            credish_rwlock_wrunlock(&store->lock);
            Py_DECREF(result);
            PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
            return NULL;
        }
        dictIterator *it = dict_iter_new((dict *)o->ptr);
        if (it) {
            dictEntry *e;
            while ((e = dict_iter_next(it))) {
                sds k = (sds)e->key;
                sds v = (sds)e->v.val;
                PyObject *pk = field_key_to_py(k, (Py_ssize_t)SDS_LEN(k), native);
                PyObject *pv = pk ? (native ? tagged_value_to_native(v) : tagged_value_to_bytes(v)) : NULL;
                if (pk && pv) PyDict_SetItem(result, pk, pv);
                Py_XDECREF(pk);
                Py_XDECREF(pv);
            }
            dict_iter_free(it);
        }
    }
    credish_rwlock_wrunlock(&store->lock);
    return result;
}

/* Shared body for HKEYS/HVALS: collect either the dict's keys or values.
 * `native` only ever applies to values — field names are never tagged. */
static PyObject *hash_collect(credish_store *store, PyObject *handle, char *key, int keylen,
                               int want_values, int native) {
    credish_rwlock_wrlock(&store->lock);
    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup(db, key, keylen);
    PyObject *result = PyList_New(0);
    if (!result) { credish_rwlock_wrunlock(&store->lock); return NULL; }
    if (o) {
        if (o->type != OBJ_HASH) {
            credish_rwlock_wrunlock(&store->lock);
            Py_DECREF(result);
            PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
            return NULL;
        }
        dictIterator *it = dict_iter_new((dict *)o->ptr);
        if (it) {
            dictEntry *e;
            while ((e = dict_iter_next(it))) {
                PyObject *item;
                if (want_values) {
                    sds v = (sds)e->v.val;
                    item = native ? tagged_value_to_native(v) : tagged_value_to_bytes(v);
                } else {
                    sds k = (sds)e->key;
                    item = field_key_to_py(k, (Py_ssize_t)SDS_LEN(k), native);
                }
                if (item) { PyList_Append(result, item); Py_DECREF(item); }
            }
            dict_iter_free(it);
        }
    }
    credish_rwlock_wrunlock(&store->lock);
    return result;
}

PyObject *py_hkeys(PyObject *self, PyObject *args, PyObject *kw) {
    (void)self;
    static char *kwlist[] = {"handle","key","native",NULL};
    PyObject *handle, *key_obj;
    int native = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|p", kwlist, &handle, &key_obj, &native))
        return NULL;
    credish_store *store = credish_get_store(handle); if (!store) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    return hash_collect(store, handle, key, keylen, 0, native);
}

PyObject *py_hvals(PyObject *self, PyObject *args, PyObject *kw) {
    (void)self;
    static char *kwlist[] = {"handle","key","native",NULL};
    PyObject *handle, *key_obj;
    int native = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|p", kwlist, &handle, &key_obj, &native))
        return NULL;
    credish_store *store = credish_get_store(handle); if (!store) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    return hash_collect(store, handle, key, keylen, 1, native);
}

PyObject *py_hlen(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj;
    if (!PyArg_ParseTuple(args, "OO", &handle, &key_obj)) return NULL;
    credish_store *store = credish_get_store(handle); if (!store) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    credish_rwlock_wrlock(&store->lock);
    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup(db, key, keylen);
    size_t sz = 0;
    if (o) {
        if (o->type != OBJ_HASH) {
            credish_rwlock_wrunlock(&store->lock);
            PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
            return NULL;
        }
        sz = dict_size((dict *)o->ptr);
    }
    credish_rwlock_wrunlock(&store->lock);
    return PyLong_FromSsize_t((Py_ssize_t)sz);
}

PyObject *py_hincrby(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj, *field_obj;
    long long amount;
    if (!PyArg_ParseTuple(args, "OOOL", &handle, &key_obj, &field_obj, &amount)) return NULL;
    credish_store *store = credish_get_store(handle); if (!store) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    sds field = pyobj_to_sds(field_obj);
    if (!field) return NULL;

    credish_rwlock_wrlock(&store->lock);
    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup_write(db, key, keylen);
    if (!o) {
        o = obj_create_hash();
        if (o) db_set(db, key, keylen, o, store);
    } else if (o->type != OBJ_HASH) {
        credish_rwlock_wrunlock(&store->lock);
        sds_free(field);
        PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
        return NULL;
    }
    if (!o) {
        credish_rwlock_wrunlock(&store->lock);
        sds_free(field);
        return PyErr_NoMemory();
    }

    dict *d = (dict *)o->ptr;
    sds existing = dict_fetch_value(d, field);
    long long val = 0;

    if (existing) {
        /* Skip the 1-byte encoding tag ahead of the payload. */
        size_t vlen = SDS_LEN(existing);
        size_t paylen = vlen > 0 ? vlen - 1 : 0;
        const char *payload = existing + 1;
        char tmp[64];
        size_t cplen = paylen < 63 ? paylen : 63;
        memcpy(tmp, payload, cplen);
        tmp[cplen] = '\0';
        char *end;
        val = strtoll(tmp, &end, 10);
        if (*end != '\0') {
            credish_rwlock_wrunlock(&store->lock);
            sds_free(field);
            PyErr_SetString(PyExc_ValueError, "hash value is not an integer");
            return NULL;
        }
    }
    val += amount;

    char buf[24];
    int n = snprintf(buf, sizeof(buf), "%lld", val);
    sds newval = sds_newlen(NULL, (size_t)n + 1);
    if (newval) {
        newval[0] = (char)OBJ_ENCODING_INT;
        memcpy(newval + 1, buf, (size_t)n);
        dict_replace(d, field, newval);
    }
    credish_rwlock_wrunlock(&store->lock);

    if (newval) {
        const char *aof_argv[] = { key, field, newval };
        size_t aof_lens[] = { (size_t)keylen, SDS_LEN(field), SDS_LEN(newval) };
        aof_append_len(store, "HSET", 3, aof_argv, aof_lens);
        sds_free(newval);
    }

    sds_free(field);
    return PyLong_FromLongLong(val);
}

