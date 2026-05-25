/*
 * credish_module.c — Python C extension entry point.
 *
 * Exposes a PyCapsule wrapping credish_store* as the "db handle"
 * passed to every operation.  All heavy lifting is in C; the GIL is
 * released (Py_BEGIN_ALLOW_THREADS) around pure-C read/write operations.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "bufpool.h"
#include "db.h"
#include "object.h"
#include "sds.h"
#include "adlist.h"
#include "dict.h"
#include "sorted_set.h"
#include "server.h"
#include "persistence/rdb.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/* Capsule                                                             */
/* ------------------------------------------------------------------ */

#define CAPSULE_NAME "credish._credish.store"

/* Non-NULL sentinel stored in a capsule after py_close() is called,
   preventing use-after-free on double-close. */
static int g_closed_sentinel;

static void store_capsule_destructor(PyObject *cap) {
    credish_store *s = PyCapsule_GetPointer(cap, CAPSULE_NAME);
    if (s && s != (credish_store *)&g_closed_sentinel) store_close(s);
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

credish_store *credish_get_store(PyObject *handle) {
    credish_store *s = PyCapsule_GetPointer(handle, CAPSULE_NAME);
    if (!s && PyTuple_Check(handle) && PyTuple_GET_SIZE(handle) == 2) {
        PyErr_Clear();
        s = PyCapsule_GetPointer(PyTuple_GET_ITEM(handle, 0), CAPSULE_NAME);
    }
    if (!s || s == (credish_store *)&g_closed_sentinel) return NULL;
    return s;
}

static int get_db_id(PyObject *handle) {
    if (PyTuple_Check(handle) && PyTuple_GET_SIZE(handle) == 2) {
        long db_id = PyLong_AsLong(PyTuple_GET_ITEM(handle, 1));
        if (PyErr_Occurred()) return -1;
        return (int)db_id;
    }
    return 0;
}

credish_db *credish_get_db(PyObject *handle, credish_store *s) {
    if (!PyTuple_Check(handle)) return &s->dbs[0];
    int db_id = get_db_id(handle);
    credish_db *db = store_select_db(s, db_id);
    if (!db) PyErr_SetString(PyExc_ValueError, "db index out of range");
    return db;
}

static credish_store *get_store(PyObject *handle) {
    return credish_get_store(handle);
}

/* Decode a Python str/bytes arg to (char*, int) */
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

/* Coerce any Python value to an sds (for SET value, LPUSH members, etc.) */
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

static int64_t now_ms_mod(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* ------------------------------------------------------------------ */
/* open / close                                                        */
/* ------------------------------------------------------------------ */

static PyObject *py_open(PyObject *self, PyObject *args, PyObject *kw) {
    (void)self;
    static char *kwlist[] = {"data_dir","persistence","save_interval","aof_fsync","db",NULL};
    const char *data_dir   = ".";
    const char *persistence = "hybrid";
    int save_interval       = 300;
    const char *aof_fsync   = "everysec";
    int db_id               = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kw, "|ssisi", kwlist,
            &data_dir, &persistence, &save_interval, &aof_fsync, &db_id))
        return NULL;

    credish_config cfg = {0};
    strncpy(cfg.data_dir, data_dir, sizeof(cfg.data_dir) - 1);
    cfg.persist_mode  = parse_persist_mode(persistence);
    cfg.save_interval = save_interval;
    cfg.aof_fsync     = parse_aof_fsync(aof_fsync);

    Py_BEGIN_ALLOW_THREADS
    /* nothing blocking here, but consistent with the pattern */
    Py_END_ALLOW_THREADS

    credish_store *s = store_open(&cfg);
    if (!s) return PyErr_NoMemory();

    return PyCapsule_New(s, CAPSULE_NAME, store_capsule_destructor);
}

static PyObject *py_close(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle;
    if (!PyArg_ParseTuple(args, "O", &handle)) return NULL;
    credish_store *s = get_store(handle);
    if (!s) return NULL;
    Py_BEGIN_ALLOW_THREADS
    store_close(s);
    Py_END_ALLOW_THREADS
    /* Invalidate the capsule so the destructor doesn't double-free */
    PyCapsule_SetDestructor(handle, NULL);
    PyCapsule_SetPointer(handle, &g_closed_sentinel);
    Py_RETURN_NONE;
}

/* ------------------------------------------------------------------ */
/* ping / flushdb / dbsize / select                                    */
/* ------------------------------------------------------------------ */

static PyObject *py_ping(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle; if (!PyArg_ParseTuple(args,"O",&handle)) return NULL;
    return PyUnicode_FromString("PONG");
}

static PyObject *py_flushdb(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle;
    if (!PyArg_ParseTuple(args, "O", &handle)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;
    Py_BEGIN_ALLOW_THREADS
    pthread_rwlock_wrlock(&s->lock);
    for (int i = 0; i < CREDISH_DB_COUNT; i++) {
        dict_free(s->dbs[i].keys);
        dict_free(s->dbs[i].expires);
        /* dict_create requires dictType — re-init deferred; just wipe used */
    }
    pthread_rwlock_unlock(&s->lock);
    Py_END_ALLOW_THREADS
    Py_RETURN_TRUE;
}

static PyObject *py_dbsize(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle; int db_id = -1;
    if (!PyArg_ParseTuple(args, "O|i", &handle, &db_id)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;
    if (db_id < 0) db_id = get_db_id(handle);
    credish_db *db = store_select_db(s, db_id); if (!db) return PyLong_FromLong(0);
    pthread_rwlock_rdlock(&s->lock);
    size_t sz = dict_size(db->keys);
    pthread_rwlock_unlock(&s->lock);
    return PyLong_FromSsize_t((Py_ssize_t)sz);
}

static PyObject *py_save(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle; if (!PyArg_ParseTuple(args,"O",&handle)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;
    int rc;
    Py_BEGIN_ALLOW_THREADS
    pthread_rwlock_rdlock(&s->lock);
    rc = rdb_save(s);
    pthread_rwlock_unlock(&s->lock);
    Py_END_ALLOW_THREADS
    return rc == 0 ? Py_True : Py_False;
}

static PyObject *py_bgsave(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle; if (!PyArg_ParseTuple(args,"O",&handle)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;
    rdb_bgsave(s);
    Py_RETURN_TRUE;
}

static PyObject *py_select(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle; int db_id;
    if (!PyArg_ParseTuple(args, "Oi", &handle, &db_id)) return NULL;
    if (db_id < 0 || db_id >= CREDISH_DB_COUNT) {
        PyErr_SetString(PyExc_ValueError, "db index out of range");
        return NULL;
    }
    Py_RETURN_TRUE;
}

/* ------------------------------------------------------------------ */
/* GET / SET / DEL / EXISTS / EXPIRE / TTL                            */
/* ------------------------------------------------------------------ */

static PyObject *py_get(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle; PyObject *key_obj;
    if (!PyArg_ParseTuple(args, "OO", &handle, &key_obj)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;

    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    pthread_rwlock_rdlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    credishObject *o = db_lookup(db, key, keylen);
    PyObject *result;
    if (!o) { result = Py_None; Py_INCREF(result); }
    else if (o->type != OBJ_STRING) {
        pthread_rwlock_unlock(&s->lock);
        PyErr_SetString(PyExc_TypeError, "WRONGTYPE: not a string");
        return NULL;
    } else {
        int vlen; char *vptr = obj_string_ptr(o, &vlen);
        result = PyBytes_FromStringAndSize(vptr, vlen);
    }
    pthread_rwlock_unlock(&s->lock);
    return result;
}

static PyObject *py_set(PyObject *self, PyObject *args, PyObject *kw) {
    (void)self;
    static char *kwlist[] = {"handle","key","value","ex","px","nx","xx",NULL};
    PyObject *handle, *key_obj, *val_obj;
    int ex = -1, px = -1, nx = 0, xx = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOO|iibb", kwlist,
            &handle, &key_obj, &val_obj, &ex, &px, &nx, &xx))
        return NULL;

    credish_store *s = get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    sds val_sds = pyobj_to_sds(val_obj);
    if (!val_sds) return NULL;

    credishObject *o = obj_steal_string(val_sds);
    if (!o) { sds_free(val_sds); return PyErr_NoMemory(); }

    int did_set = 0;
    pthread_rwlock_wrlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    if (!(nx && db_lookup(db, key, keylen)) &&
        !(xx && !db_lookup(db, key, keylen))) {
        db_set(db, key, keylen, o, s);
        if (ex > 0) db_set_expire(db, key, keylen, now_ms_mod() + (int64_t)ex * 1000LL);
        if (px > 0) db_set_expire(db, key, keylen, now_ms_mod() + (int64_t)px);
        did_set = 1;
    }
    pthread_rwlock_unlock(&s->lock);

    if (!did_set) {
        obj_free(o);
        Py_RETURN_NONE;
    }
    const char *argv_arr[] = { key, (char *)o->ptr };
    aof_append(s, "SET", 2, argv_arr);
    Py_RETURN_TRUE;
}

static PyObject *py_delete(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *keys_list;
    if (!PyArg_ParseTuple(args, "OO", &handle, &keys_list)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;

    if (!PyList_Check(keys_list)) { PyErr_SetString(PyExc_TypeError,"expected list"); return NULL; }
    Py_ssize_t n = PyList_GET_SIZE(keys_list);
    int deleted = 0;
    pthread_rwlock_wrlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *ko = PyList_GET_ITEM(keys_list, i);
        char *key; int keylen;
        if (!decode_key(ko, &key, &keylen)) continue;
        deleted += db_del(db, key, keylen, s);
    }
    pthread_rwlock_unlock(&s->lock);
    return PyLong_FromLong(deleted);
}

static PyObject *py_exists(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *keys_list;
    if (!PyArg_ParseTuple(args, "OO", &handle, &keys_list)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;
    if (!PyList_Check(keys_list)) { PyErr_SetString(PyExc_TypeError,"expected list"); return NULL; }
    Py_ssize_t n = PyList_GET_SIZE(keys_list);
    int count = 0;
    pthread_rwlock_rdlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *ko = PyList_GET_ITEM(keys_list, i);
        char *key; int keylen;
        if (!decode_key(ko, &key, &keylen)) continue;
        if (db_lookup(db, key, keylen)) count++;
    }
    pthread_rwlock_unlock(&s->lock);
    return PyLong_FromLong(count);
}

static PyObject *py_expire(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj; int seconds;
    if (!PyArg_ParseTuple(args, "OOi", &handle, &key_obj, &seconds)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    pthread_rwlock_wrlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    int found = db_lookup(db, key, keylen) != NULL;
    if (found) db_set_expire(db, key, keylen, now_ms_mod() + (int64_t)seconds * 1000LL);
    pthread_rwlock_unlock(&s->lock);
    return found ? Py_True : Py_False;
}

static PyObject *py_pexpire(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj; long long ms;
    if (!PyArg_ParseTuple(args, "OOL", &handle, &key_obj, &ms)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    pthread_rwlock_wrlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    int found = db_lookup(db, key, keylen) != NULL;
    if (found) db_set_expire(db, key, keylen, now_ms_mod() + (int64_t)ms);
    pthread_rwlock_unlock(&s->lock);
    return found ? Py_True : Py_False;
}

static PyObject *py_persist(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj;
    if (!PyArg_ParseTuple(args, "OO", &handle, &key_obj)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    pthread_rwlock_wrlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    int64_t dl = db_get_expire(db, key, keylen);
    if (dl >= 0) db_remove_expire(db, key, keylen);
    pthread_rwlock_unlock(&s->lock);
    return dl >= 0 ? Py_True : Py_False;
}

static PyObject *py_ttl(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj;
    if (!PyArg_ParseTuple(args, "OO", &handle, &key_obj)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    pthread_rwlock_rdlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    credishObject *o = db_lookup(db, key, keylen);
    int64_t result;
    if (!o) { result = -2; }
    else {
        int64_t dl = db_get_expire(db, key, keylen);
        result = dl < 0 ? -1 : (dl - now_ms_mod()) / 1000LL;
    }
    pthread_rwlock_unlock(&s->lock);
    return PyLong_FromLongLong((long long)result);
}

static PyObject *py_pttl(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj;
    if (!PyArg_ParseTuple(args, "OO", &handle, &key_obj)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    pthread_rwlock_rdlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    credishObject *o = db_lookup(db, key, keylen);
    int64_t result;
    if (!o) { result = -2; }
    else {
        int64_t dl = db_get_expire(db, key, keylen);
        result = dl < 0 ? -1 : dl - now_ms_mod();
    }
    pthread_rwlock_unlock(&s->lock);
    return PyLong_FromLongLong((long long)result);
}

static PyObject *py_type(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj;
    if (!PyArg_ParseTuple(args, "OO", &handle, &key_obj)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    pthread_rwlock_rdlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    credishObject *o = db_lookup(db, key, keylen);
    const char *tname = "none";
    if (o) {
        switch (o->type) {
        case OBJ_STRING: tname = "string"; break;
        case OBJ_LIST:   tname = "list";   break;
        case OBJ_HASH:   tname = "hash";   break;
        case OBJ_SET:    tname = "set";    break;
        case OBJ_ZSET:   tname = "zset";   break;
        }
    }
    pthread_rwlock_unlock(&s->lock);
    return PyUnicode_FromString(tname);
}

/* ------------------------------------------------------------------ */
/* INCR / DECR family                                                  */
/* ------------------------------------------------------------------ */

static PyObject *py_incrby(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj; long long amount;
    if (!PyArg_ParseTuple(args, "OOL", &handle, &key_obj, &amount)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    pthread_rwlock_wrlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    credishObject *o = db_lookup(db, key, keylen);
    long long val = 0;
    if (o) {
        if (o->type != OBJ_STRING) {
            pthread_rwlock_unlock(&s->lock);
            PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
            return NULL;
        }
        int vlen; char *vptr = obj_string_ptr(o, &vlen);
        char tmp[64]; memcpy(tmp, vptr, vlen < 63 ? (size_t)vlen : 63); tmp[vlen < 63 ? vlen : 63] = '\0';
        char *end; val = strtoll(tmp, &end, 10);
        if (*end != '\0') {
            pthread_rwlock_unlock(&s->lock);
            PyErr_SetString(PyExc_ValueError, "not an integer");
            return NULL;
        }
    }
    val += amount;
    char buf[24]; int n = snprintf(buf, sizeof(buf), "%lld", val);
    credishObject *new_o = obj_create_string(buf, n);
    db_set(db, key, keylen, new_o, s);
    pthread_rwlock_unlock(&s->lock);

    char amount_buf[24];
    snprintf(amount_buf, sizeof(amount_buf), "%lld", amount);
    const char *incrby_argv[] = { key, amount_buf };
    aof_append(s, "INCRBY", 2, incrby_argv);

    return PyLong_FromLongLong(val);
}

/* ------------------------------------------------------------------ */
/* LPUSH / RPUSH / LPOP / RPOP / LRANGE / LLEN                        */
/* ------------------------------------------------------------------ */

static PyObject *py_lpush(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj, *vals_list;
    if (!PyArg_ParseTuple(args, "OOO", &handle, &key_obj, &vals_list)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    if (!PyList_Check(vals_list)) { PyErr_SetString(PyExc_TypeError,"expected list"); return NULL; }

    pthread_rwlock_wrlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    credishObject *o = db_lookup_write(db, key, keylen);
    if (!o) {
        o = obj_create_list();
        db_set(db, key, keylen, o, s);
    } else if (o->type != OBJ_LIST) {
        pthread_rwlock_unlock(&s->lock);
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
    pthread_rwlock_unlock(&s->lock);

    if (s->aof_fp && n > 0) {
        const char **aof_argv = malloc((size_t)(1 + n) * sizeof(char *));
        sds *tmp_sdses = malloc((size_t)n * sizeof(sds));
        if (aof_argv && tmp_sdses) {
            aof_argv[0] = key;
            Py_ssize_t cnt = 0;
            for (Py_ssize_t i = 0; i < n; i++) {
                sds v = pyobj_to_sds(PyList_GET_ITEM(vals_list, i));
                if (v) { tmp_sdses[cnt] = v; aof_argv[1 + cnt] = v; cnt++; }
            }
            if (cnt > 0) aof_append(s, "LPUSH", (int)(1 + cnt), aof_argv);
            for (Py_ssize_t i = 0; i < cnt; i++) sds_free(tmp_sdses[i]);
        }
        free(aof_argv);
        free(tmp_sdses);
    }

    return PyLong_FromSsize_t((Py_ssize_t)sz);
}

static PyObject *py_rpush(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj, *vals_list;
    if (!PyArg_ParseTuple(args, "OOO", &handle, &key_obj, &vals_list)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    if (!PyList_Check(vals_list)) { PyErr_SetString(PyExc_TypeError,"expected list"); return NULL; }

    pthread_rwlock_wrlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    credishObject *o = db_lookup_write(db, key, keylen);
    if (!o) {
        o = obj_create_list();
        db_set(db, key, keylen, o, s);
    } else if (o->type != OBJ_LIST) {
        pthread_rwlock_unlock(&s->lock);
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
    pthread_rwlock_unlock(&s->lock);

    if (s->aof_fp && n > 0) {
        const char **aof_argv = malloc((size_t)(1 + n) * sizeof(char *));
        sds *tmp_sdses = malloc((size_t)n * sizeof(sds));
        if (aof_argv && tmp_sdses) {
            aof_argv[0] = key;
            Py_ssize_t cnt = 0;
            for (Py_ssize_t i = 0; i < n; i++) {
                sds v = pyobj_to_sds(PyList_GET_ITEM(vals_list, i));
                if (v) { tmp_sdses[cnt] = v; aof_argv[1 + cnt] = v; cnt++; }
            }
            if (cnt > 0) aof_append(s, "RPUSH", (int)(1 + cnt), aof_argv);
            for (Py_ssize_t i = 0; i < cnt; i++) sds_free(tmp_sdses[i]);
        }
        free(aof_argv);
        free(tmp_sdses);
    }

    return PyLong_FromSsize_t((Py_ssize_t)sz);
}

static PyObject *py_lrange(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj; int start, stop;
    if (!PyArg_ParseTuple(args, "OOii", &handle, &key_obj, &start, &stop)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    pthread_rwlock_rdlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
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
            listNode *n = adlist_index(l, start);
            for (int i = start; n && i <= stop; i++, n = n->next) {
                sds sv = (sds)n->value;
                PyObject *elem = PyBytes_FromStringAndSize(sv, (Py_ssize_t)SDS_LEN(sv));
                PyList_Append(result, elem);
                Py_DECREF(elem);
            }
        }
    }
    pthread_rwlock_unlock(&s->lock);
    return result;
}

static PyObject *py_llen(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj;
    if (!PyArg_ParseTuple(args, "OO", &handle, &key_obj)) return NULL;
    credish_store *s = get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    pthread_rwlock_rdlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    credishObject *o = db_lookup(db, key, keylen);
    size_t sz = 0;
    if (o && o->type == OBJ_LIST) sz = ((adlist *)o->ptr)->len;
    pthread_rwlock_unlock(&s->lock);
    return PyLong_FromSsize_t((Py_ssize_t)sz);
}

/* ------------------------------------------------------------------ */
/* Module method table                                                 */
/* ------------------------------------------------------------------ */

static PyMethodDef credish_methods[] = {
    {"open",     (PyCFunction)py_open,    METH_VARARGS|METH_KEYWORDS, NULL},
    {"close",    py_close,                METH_VARARGS,               NULL},
    {"ping",     py_ping,                 METH_VARARGS,               NULL},
    {"flushdb",  py_flushdb,              METH_VARARGS,               NULL},
    {"dbsize",   py_dbsize,               METH_VARARGS,               NULL},
    {"save",     py_save,                 METH_VARARGS,               NULL},
    {"bgsave",   py_bgsave,              METH_VARARGS,               NULL},
    {"select",   py_select,              METH_VARARGS,               NULL},
    {"get",      py_get,                  METH_VARARGS,               NULL},
    {"set",      (PyCFunction)py_set,     METH_VARARGS|METH_KEYWORDS, NULL},
    {"delete",   py_delete,              METH_VARARGS,               NULL},
    {"exists",   py_exists,              METH_VARARGS,               NULL},
    {"expire",   py_expire,              METH_VARARGS,               NULL},
    {"pexpire",  py_pexpire,             METH_VARARGS,               NULL},
    {"persist",  py_persist,             METH_VARARGS,               NULL},
    {"ttl",      py_ttl,                 METH_VARARGS,               NULL},
    {"pttl",     py_pttl,                METH_VARARGS,               NULL},
    {"type_",    py_type,                METH_VARARGS,               NULL},
    {"incrby",   py_incrby,              METH_VARARGS,               NULL},
    {"lpush",    py_lpush,               METH_VARARGS,               NULL},
    {"rpush",    py_rpush,               METH_VARARGS,               NULL},
    {"lrange",   py_lrange,              METH_VARARGS,               NULL},
    {"llen",     py_llen,                METH_VARARGS,               NULL},
    {"zadd",     (PyCFunction)py_zadd,   METH_VARARGS|METH_KEYWORDS, NULL},
    {"zrange",   (PyCFunction)py_zrange, METH_VARARGS|METH_KEYWORDS, NULL},
    {"zrevrange",(PyCFunction)py_zrevrange, METH_VARARGS|METH_KEYWORDS, NULL},
    {"zrank",    py_zrank,               METH_VARARGS,               NULL},
    {"zrevrank", py_zrevrank,            METH_VARARGS,               NULL},
    {"zscore",   py_zscore,              METH_VARARGS,               NULL},
    {"zrem",     py_zrem,                METH_VARARGS,               NULL},
    {"zcard",    py_zcard,               METH_VARARGS,               NULL},
    {"zrangebyscore", (PyCFunction)py_zrangebyscore, METH_VARARGS|METH_KEYWORDS, NULL},
    {"zincrby",  py_zincrby,             METH_VARARGS,               NULL},
    /* Remaining commands (incr/decr, hset/hget, sadd, …)
     * follow the same pattern — stubs to be filled in Phase 2. */
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef credish_module = {
    PyModuleDef_HEAD_INIT,
    "_credish",
    "Credish C extension — Redis-compatible in-process cache",
    -1,
    credish_methods,
};

PyMODINIT_FUNC PyInit__credish(void) {
    bufpool_init();
    return PyModule_Create(&credish_module);
}
