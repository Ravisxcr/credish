#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "hash.h"
#include "object.h"
#include "sds.h"
#include "dict.h"
#include "platform.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>


/* Decode a Python str/bytes arg to (char*, int). */
static int decode_key(PyObject *obj, char **out, int *out_len) {

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


static sds pyobj_to_sds(PyObject *obj) {

    if (PyBytes_Check(obj))
        return sds_newlen(PyBytes_AS_STRING(obj), (size_t)PyBytes_GET_SIZE(obj));

    if (PyUnicode_Check(obj)) {
        Py_ssize_t sz;
        const char *s = PyUnicode_AsUTF8AndSize(obj, &sz);
        return s ? sds_newlen(s, (size_t)sz) : NULL;
    }

    if (PyLong_Check(obj)) {
        long long v = PyLong_AsLongLong(obj);
        char buf[24];
        int n = snprintf(buf, sizeof(buf), "%lld", v);
        return sds_newlen(buf, (size_t)n);
    }

    if (PyFloat_Check(obj)) {
        double v = PyFloat_AS_DOUBLE(obj);
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%.17g", v);
        return sds_newlen(buf, (size_t)n);
    }

    PyErr_SetString(PyExc_TypeError, "value must be str, bytes, int, or float");

    return NULL;
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
        sds v = f ? pyobj_to_sds(mv) : NULL;
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
    if (aof_argv) {
        aof_argv[0] = key;
        for (Py_ssize_t i = 0; i < n; i++) {
            aof_argv[1 + 2 * i] = fields[i];
            aof_argv[2 + 2 * i] = values[i];
        }
        aof_append(store, "HSET", (int)(1 + 2 * n), aof_argv);
        free(aof_argv);
    }

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
        sds v = f ? pyobj_to_sds(value_obj) : NULL;
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

PyObject *py_hget(PyObject *self, PyObject *args) {
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
            result = PyBytes_FromStringAndSize(val, (Py_ssize_t)SDS_LEN(val));
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

PyObject *py_hmget(PyObject *self, PyObject *args) {
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
            item = PyBytes_FromStringAndSize(val, (Py_ssize_t)SDS_LEN(val));
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
        if (aof_argv) {
            aof_argv[0] = key;
            for (Py_ssize_t i = 0; i < removed_count; i++) aof_argv[1 + i] = removed_fields[i];
            aof_append(store, "HDEL", (int)(1 + removed_count), aof_argv);
            free(aof_argv);
        }
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

PyObject *py_hgetall(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj;
    if (!PyArg_ParseTuple(args, "OO", &handle, &key_obj)) return NULL;
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
                PyObject *pk = PyBytes_FromStringAndSize(k, (Py_ssize_t)SDS_LEN(k));
                PyObject *pv = pk ? PyBytes_FromStringAndSize(v, (Py_ssize_t)SDS_LEN(v)) : NULL;
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

/* Shared body for HKEYS/HVALS: collect either the dict's keys or values. */
static PyObject *hash_collect(credish_store *store, PyObject *handle, char *key, int keylen, int want_values) {
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
                sds s = want_values ? (sds)e->v.val : (sds)e->key;
                PyObject *item = PyBytes_FromStringAndSize(s, (Py_ssize_t)SDS_LEN(s));
                if (item) { PyList_Append(result, item); Py_DECREF(item); }
            }
            dict_iter_free(it);
        }
    }
    credish_rwlock_wrunlock(&store->lock);
    return result;
}

PyObject *py_hkeys(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj;
    if (!PyArg_ParseTuple(args, "OO", &handle, &key_obj)) return NULL;
    credish_store *store = credish_get_store(handle); if (!store) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    return hash_collect(store, handle, key, keylen, 0);
}

PyObject *py_hvals(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj;
    if (!PyArg_ParseTuple(args, "OO", &handle, &key_obj)) return NULL;
    credish_store *store = credish_get_store(handle); if (!store) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    return hash_collect(store, handle, key, keylen, 1);
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
        char tmp[64];
        size_t vlen = SDS_LEN(existing);
        size_t cplen = vlen < 63 ? vlen : 63;
        memcpy(tmp, existing, cplen);
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
    sds newval = sds_newlen(buf, (size_t)n);
    if (newval) {
        dict_replace(d, field, newval);
        sds_free(newval);
    }
    credish_rwlock_wrunlock(&store->lock);

    const char *aof_argv[] = { key, field, buf };
    aof_append(store, "HSET", 3, aof_argv);

    sds_free(field);
    return PyLong_FromLongLong(val);
}
