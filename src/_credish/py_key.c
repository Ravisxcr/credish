#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "py_key.h"
#include "py_helpers.h"
#include "db.h"
#include "object.h"
#include "platform.h"
#include <string.h>

PyObject *py_delete(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle;
    PyObject *keys_list;

    if (!PyArg_ParseTuple(args, "OO", &handle, &keys_list)) return NULL;

    credish_store *store = credish_get_store(handle); if (!store) return NULL;

    if (!PyList_Check(keys_list)) {
        PyErr_SetString(PyExc_TypeError,"expected list");
        return NULL;
    }

    Py_ssize_t n = PyList_GET_SIZE(keys_list);
    int deleted = 0;

    credish_rwlock_wrlock(&store->lock);

    credish_db *db = credish_get_db(handle, store);

    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *ko = PyList_GET_ITEM(keys_list, i);
        char *key;
        int keylen;

        if (!decode_key(ko, &key, &keylen)) continue;
        deleted += db_del(db, key, keylen, store);
    }

    credish_rwlock_wrunlock(&store->lock);

    return PyLong_FromLong(deleted);
}

PyObject *py_exists(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle;
    PyObject *keys_list;

    if (!PyArg_ParseTuple(args, "OO", &handle, &keys_list)) return NULL;

    credish_store *store = credish_get_store(handle); if (!store) return NULL;

    if (!PyList_Check(keys_list)) {
        PyErr_SetString(PyExc_TypeError,"expected list");
        return NULL;
    }

    Py_ssize_t n = PyList_GET_SIZE(keys_list);
    int count = 0;

    credish_rwlock_wrlock(&store->lock);

    credish_db *db = credish_get_db(handle, store);

    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *ko = PyList_GET_ITEM(keys_list, i);
        char *key; 
        int keylen;

        if (!decode_key(ko, &key, &keylen)) continue;
        if (db_lookup(db, key, keylen)) count++;
    }

    credish_rwlock_wrunlock(&store->lock);

    return PyLong_FromLong(count);
}

PyObject *py_expire(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle;
    PyObject *key_obj; 
    int seconds;

    if (!PyArg_ParseTuple(args, "OOi", &handle, &key_obj, &seconds)) return NULL;

    credish_store *store = credish_get_store(handle); if (!store) return NULL;

    char *key; 
    int keylen;

    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    credish_rwlock_wrlock(&store->lock);

    credish_db *db = credish_get_db(handle, store);
    int found = db_lookup(db, key, keylen) != NULL;
    if (found) db_set_expire(db, key, keylen, now_ms_mod() + (int64_t)seconds * 1000LL);

    credish_rwlock_wrunlock(&store->lock);

    return found ? Py_True : Py_False;
}

PyObject *py_pexpire(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle;
    PyObject *key_obj;
    long long ms;

    if (!PyArg_ParseTuple(args, "OOL", &handle, &key_obj, &ms)) return NULL;

    credish_store *store = credish_get_store(handle); if (!store) return NULL;

    char *key; 
    int keylen;

    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    credish_rwlock_wrlock(&store->lock);

    credish_db *db = credish_get_db(handle, store);
    int found = db_lookup(db, key, keylen) != NULL;
    if (found) db_set_expire(db, key, keylen, now_ms_mod() + (int64_t)ms);

    credish_rwlock_wrunlock(&store->lock);

    return found ? Py_True : Py_False;
}

PyObject *py_persist(PyObject *self, PyObject *args) {
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
    int64_t dl = db_get_expire(db, key, keylen);
    if (dl >= 0) db_remove_expire(db, key, keylen);

    credish_rwlock_wrunlock(&store->lock);

    return dl >= 0 ? Py_True : Py_False;
}

PyObject *py_ttl(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle;
    PyObject *key_obj;

    if (!PyArg_ParseTuple(args, "OO", &handle, &key_obj)) return NULL;

    credish_store *store = credish_get_store(handle); if (!store) return NULL;

    char *key;
    int keylen;
    int64_t result;

    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    credish_rwlock_wrlock(&store->lock);

    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup(db, key, keylen);
    
    if (!o) {
        result = -2; 
    } else {
        int64_t dl = db_get_expire(db, key, keylen);
        result = dl < 0 ? -1 : (dl - now_ms_mod()) / 1000LL;
    }

    credish_rwlock_wrunlock(&store->lock);

    return PyLong_FromLongLong((long long)result);
}

PyObject *py_pttl(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle;
    PyObject *key_obj;

    if (!PyArg_ParseTuple(args, "OO", &handle, &key_obj)) return NULL;

    credish_store *store = credish_get_store(handle); if (!store) return NULL;

    char *key;
    int keylen;
    int64_t result;

    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    credish_rwlock_wrlock(&store->lock);

    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup(db, key, keylen);
    
    if (!o) { 
        result = -2; 
    } else {
        int64_t dl = db_get_expire(db, key, keylen);
        result = dl < 0 ? -1 : dl - now_ms_mod();
    }

    credish_rwlock_wrunlock(&store->lock);

    return PyLong_FromLongLong((long long)result);
}

PyObject *py_type(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle;
    PyObject *key_obj;

    if (!PyArg_ParseTuple(args, "OO", &handle, &key_obj)) return NULL;

    credish_store *store = credish_get_store(handle); if (!store) return NULL;

    char *key;
    int keylen;
    const char *tname = "none";

    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    credish_rwlock_wrlock(&store->lock);

    credish_db *db = credish_get_db(handle, store);
    credishObject *o = db_lookup(db, key, keylen);
    
    if (o) {
        switch (o->type) {
        case OBJ_STRING: 
            tname = "string";
            break;
        case OBJ_LIST:   
            tname = "list";   
            break;
        case OBJ_HASH:   
            tname = "hash";   
            break;
        case OBJ_SET:    
            tname = "set";    
            break;
        case OBJ_ZSET:   
            tname = "zset";   
            break;
        }
    }

    credish_rwlock_wrunlock(&store->lock);

    return PyUnicode_FromString(tname);
}

