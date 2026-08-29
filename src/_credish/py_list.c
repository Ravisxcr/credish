#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "py_list.h"
#include "py_helpers.h"
#include "db.h"
#include "object.h"
#include "sds.h"
#include "adlist.h"
#include "platform.h"
#include "persistence/aof.h"
#include <string.h>
#include <stdlib.h>

PyObject *py_lpush(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle;
    PyObject *key_obj;
    PyObject *vals_list;

    if (!PyArg_ParseTuple(args, "OOO", &handle, &key_obj, &vals_list)) return NULL;

    credish_store *store = credish_get_store(handle); if (!store) return NULL;

    char *key; 
    int keylen;

    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    if (!PyList_Check(vals_list)) { 
        PyErr_SetString(PyExc_TypeError,"expected list"); 
        return NULL; 
    }

    credish_rwlock_wrlock(&store->lock);

    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup_write(db, key, keylen);

    if (!o) {
        o = obj_create_list();
        db_set(db, key, keylen, o, store);
    } else if (o->type != OBJ_LIST) {
        credish_rwlock_wrunlock(&store->lock);
        PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
        return NULL;
    }

    adlist *l = (adlist *)o->ptr;
    Py_ssize_t n = PyList_GET_SIZE(vals_list);

    for (Py_ssize_t i = 0; i < n; i++) {
        sds v = pyobj_to_sds(PyList_GET_ITEM(vals_list, i));
        if (v) adlist_push_head(l, v);
    }

    size_t sz = l->len;

    credish_rwlock_wrunlock(&store->lock);

    if (store->aof_file && n > 0) {
        const char **aof_argv = malloc((size_t)(1 + n) * sizeof(char *));
        size_t *aof_lens = malloc((size_t)(1 + n) * sizeof(size_t));
        sds *tmp_sdses = malloc((size_t)n * sizeof(sds));

        if (aof_argv && aof_lens && tmp_sdses) {
            aof_argv[0] = key; aof_lens[0] = (size_t)keylen;
            Py_ssize_t cnt = 0;

            for (Py_ssize_t i = 0; i < n; i++) {
                sds v = pyobj_to_sds(PyList_GET_ITEM(vals_list, i));
                if (v) { tmp_sdses[cnt] = v; aof_argv[1 + cnt] = v; aof_lens[1 + cnt] = SDS_LEN(v); cnt++; }
            }

            if (cnt > 0) aof_append_len(store, "LPUSH", (int)(1 + cnt), aof_argv, aof_lens);

            for (Py_ssize_t i = 0; i < cnt; i++) sds_free(tmp_sdses[i]);
        }
        free(aof_argv);
        free(aof_lens);
        free(tmp_sdses);
    }

    return PyLong_FromSsize_t((Py_ssize_t)sz);
}

PyObject *py_rpush(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle;
    PyObject *key_obj;
    PyObject *vals_list;

    if (!PyArg_ParseTuple(args, "OOO", &handle, &key_obj, &vals_list)) return NULL;

    credish_store *store = credish_get_store(handle); if (!store) return NULL;

    char *key; 
    int keylen;

    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    if (!PyList_Check(vals_list)) { 
        PyErr_SetString(PyExc_TypeError,"expected list"); 
        return NULL; 
    }

    credish_rwlock_wrlock(&store->lock);

    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup_write(db, key, keylen);

    if (!o) {
        o = obj_create_list();
        db_set(db, key, keylen, o, store);
    } else if (o->type != OBJ_LIST) {
        credish_rwlock_wrunlock(&store->lock);
        PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
        return NULL;
    }

    adlist *l = (adlist *)o->ptr;
    Py_ssize_t n = PyList_GET_SIZE(vals_list);

    for (Py_ssize_t i = 0; i < n; i++) {
        sds v = pyobj_to_sds(PyList_GET_ITEM(vals_list, i));
        if (v) adlist_push_tail(l, v);
    }

    size_t sz = l->len;
    credish_rwlock_wrunlock(&store->lock);

    if (store->aof_file && n > 0) {
        const char **aof_argv = malloc((size_t)(1 + n) * sizeof(char *));
        size_t *aof_lens = malloc((size_t)(1 + n) * sizeof(size_t));
        sds *tmp_sdses = malloc((size_t)n * sizeof(sds));

        if (aof_argv && aof_lens && tmp_sdses) {
            aof_argv[0] = key; aof_lens[0] = (size_t)keylen;
            Py_ssize_t cnt = 0;

            for (Py_ssize_t i = 0; i < n; i++) {
                sds v = pyobj_to_sds(PyList_GET_ITEM(vals_list, i));
                if (v) { tmp_sdses[cnt] = v; aof_argv[1 + cnt] = v; aof_lens[1 + cnt] = SDS_LEN(v); cnt++; }
            }

            if (cnt > 0) aof_append_len(store, "RPUSH", (int)(1 + cnt), aof_argv, aof_lens);

            for (Py_ssize_t i = 0; i < cnt; i++) sds_free(tmp_sdses[i]);
        }
        free(aof_argv);
        free(aof_lens);
        free(tmp_sdses);
    }

    return PyLong_FromSsize_t((Py_ssize_t)sz);
}

PyObject *py_lrange(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle;
    PyObject *key_obj; 
    int start;
    int stop;

    if (!PyArg_ParseTuple(args, "OOii", &handle, &key_obj, &start, &stop)) return NULL;

    credish_store *store = credish_get_store(handle); if (!store) return NULL;

    char *key; 
    int keylen;

    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    credish_rwlock_wrlock(&store->lock);

    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup(db, key, keylen);
    PyObject *result = PyList_New(0);

    if (o && o->type == OBJ_LIST) {
        adlist *l = (adlist *)o->ptr;
        long len = (long)l->len;

        if (start < 0) start = (int)(len + start);
        if (stop  < 0) stop  = (int)(len + stop);
        if (start < 0) start = 0;
        if (stop >= len) stop = (int)len - 1;
        if (start <= stop) {
            adlist_node *n = adlist_index(l, start);
            for (int i = start; n && i <= stop; i++, n = n->next) {
                sds sv = (sds)n->value;
                PyObject *elem = PyBytes_FromStringAndSize(sv, (Py_ssize_t)SDS_LEN(sv));
                PyList_Append(result, elem);
                Py_DECREF(elem);
            }
        }
    }

    credish_rwlock_wrunlock(&store->lock);

    return result;
}

PyObject *py_llen(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle;
    PyObject *key_obj;

    if (!PyArg_ParseTuple(args, "OO", &handle, &key_obj)) return NULL;

    credish_store *store = credish_get_store(handle); if (!store) return NULL;

    char *key;
    int keylen;

    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    credish_rwlock_wrlock(&store->lock);

    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup(db, key, keylen);

    size_t sz = 0;

    if (o && o->type == OBJ_LIST) sz = ((adlist *)o->ptr)->len;

    credish_rwlock_wrunlock(&store->lock);

    return PyLong_FromSsize_t((Py_ssize_t)sz);
}

