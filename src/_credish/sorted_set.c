#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "sorted_set.h"
#include "object.h"
#include "sds.h"
#include "dict.h"
#include "skiplist.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>

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

static PyObject *zset_py_member_score(sds member, double score, int withscores) {
    PyObject *m = PyBytes_FromStringAndSize(member, (Py_ssize_t)SDS_LEN(member));
    if (!m) return NULL;
    if (!withscores) return m;
    PyObject *s = PyFloat_FromDouble(score);
    if (!s) { Py_DECREF(m); return NULL; }
    PyObject *t = PyTuple_Pack(2, m, s);
    Py_DECREF(m);
    Py_DECREF(s);
    return t;
}

static int zset_score_from_py(PyObject *obj, double *out) {
    double score = PyFloat_AsDouble(obj);
    if (PyErr_Occurred()) return 0;
    if (isnan(score)) {
        PyErr_SetString(PyExc_ValueError, "score is not a valid float");
        return 0;
    }
    *out = score;
    return 1;
}

static int zset_add(zset *zs, sds member, double score, int nx, int xx,
                    int gt, int lt, int *added, int *changed) {
    double *oldp = dict_fetch_value(zs->dict, member);
    *added = 0;
    *changed = 0;

    if (oldp) {
        double old = *oldp;
        if (nx) return 0;
        if ((gt && score <= old) || (lt && score >= old)) return 0;
        if (score != old) {
            zsl_delete(zs->zsl, old, member);
            zsl_insert(zs->zsl, score, sds_dup(member));
            dict_replace(zs->dict, member, &score);
            *changed = 1;
        }
        return 1;
    }

    if (xx) return 0;
    dict_replace(zs->dict, member, &score);
    zsl_insert(zs->zsl, score, sds_dup(member));
    *added = 1;
    return 1;
}

static int zset_delete_member(zset *zs, sds member) {
    double *score = dict_fetch_value(zs->dict, member);
    if (!score) return 0;
    double old = *score;
    if (!zsl_delete(zs->zsl, old, member)) return 0;
    dict_delete(zs->dict, member);
    return 1;
}

PyObject *py_zadd(PyObject *self, PyObject *args, PyObject *kw) {
    (void)self;
    static char *kwlist[] = {"handle","key","mapping","nx","xx","gt","lt","ch",NULL};
    PyObject *handle, *key_obj, *mapping;
    int nx = 0, xx = 0, gt = 0, lt = 0, ch = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOO|bbbbb", kwlist,
            &handle, &key_obj, &mapping, &nx, &xx, &gt, &lt, &ch))
        return NULL;
    if (!PyDict_Check(mapping)) {
        PyErr_SetString(PyExc_TypeError, "mapping must be a dict");
        return NULL;
    }
    if ((nx && xx) || (gt && lt) || (nx && (gt || lt))) {
        PyErr_SetString(PyExc_ValueError, "invalid ZADD options");
        return NULL;
    }

    credish_store *s = credish_get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    Py_ssize_t n = PyDict_Size(mapping);
    if (n == 0) return PyLong_FromLong(0);
    sds *members = calloc((size_t)n, sizeof(sds));
    double *scores = calloc((size_t)n, sizeof(double));
    int *applied = calloc((size_t)n, sizeof(int));
    if (!members || !scores || !applied) {
        free(members); free(scores); free(applied);
        return PyErr_NoMemory();
    }

    PyObject *mk, *mv;
    Py_ssize_t pos = 0, count = 0;
    while (PyDict_Next(mapping, &pos, &mk, &mv)) {
        sds member = pyobj_to_sds(mk);
        double score;
        if (!member || !zset_score_from_py(mv, &score)) {
            if (member) sds_free(member);
            for (Py_ssize_t i = 0; i < count; i++) sds_free(members[i]);
            free(members); free(scores); free(applied);
            return NULL;
        }
        members[count] = member;
        scores[count] = score;
        count++;
    }

    int result = 0;
    pthread_rwlock_wrlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    credishObject *o = db_lookup_write(db, key, keylen);
    if (!o) {
        o = obj_create_zset();
        if (o) db_set(db, key, keylen, o, s);
    } else if (o->type != OBJ_ZSET) {
        pthread_rwlock_unlock(&s->lock);
        for (Py_ssize_t i = 0; i < count; i++) sds_free(members[i]);
        free(members); free(scores); free(applied);
        PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
        return NULL;
    }
    if (!o) {
        pthread_rwlock_unlock(&s->lock);
        for (Py_ssize_t i = 0; i < count; i++) sds_free(members[i]);
        free(members); free(scores); free(applied);
        return PyErr_NoMemory();
    }
    zset *zs = (zset *)o->ptr;
    for (Py_ssize_t i = 0; i < count; i++) {
        int added = 0, changed = 0;
        if (zset_add(zs, members[i], scores[i], nx, xx, gt, lt, &added, &changed)) {
            applied[i] = added || changed;
            result += ch ? (added || changed) : added;
        }
    }
    pthread_rwlock_unlock(&s->lock);

    for (Py_ssize_t i = 0; i < count; i++) {
        if (applied[i]) {
            char scorebuf[64];
            snprintf(scorebuf, sizeof(scorebuf), "%.17g", scores[i]);
            const char *aof_argv[] = { key, scorebuf, members[i] };
            aof_append(s, "ZADD", 3, aof_argv);
        }
        sds_free(members[i]);
    }
    free(members); free(scores); free(applied);
    return PyLong_FromLong(result);
}

static PyObject *py_zrange_common(PyObject *self, PyObject *args, PyObject *kw, int reverse) {
    (void)self;
    static char *kwlist[] = {"handle","key","start","stop","withscores",NULL};
    PyObject *handle, *key_obj;
    int start, stop, withscores = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOii|b", kwlist,
            &handle, &key_obj, &start, &stop, &withscores))
        return NULL;
    credish_store *s = credish_get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    pthread_rwlock_wrlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    credishObject *o = db_lookup(db, key, keylen);
    if (!o) {
        pthread_rwlock_unlock(&s->lock);
        return PyList_New(0);
    }
    if (o->type != OBJ_ZSET) {
        pthread_rwlock_unlock(&s->lock);
        PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
        return NULL;
    }
    zset *zs = (zset *)o->ptr;
    long len = (long)zs->zsl->length;
    if (start < 0) start = (int)(len + start);
    if (stop < 0) stop = (int)(len + stop);
    if (start < 0) start = 0;
    if (stop >= len) stop = (int)len - 1;
    PyObject *result = PyList_New(0);
    if (result && start <= stop && len > 0) {
        if (!reverse) {
            zskiplistNode *x = zsl_get_element_by_rank(zs->zsl, (unsigned long)start + 1);
            for (int i = start; x && i <= stop; i++, x = x->level[0].forward) {
                PyObject *item = zset_py_member_score(x->member, x->score, withscores);
                if (!item || PyList_Append(result, item) < 0) { Py_XDECREF(item); Py_DECREF(result); result = NULL; break; }
                Py_DECREF(item);
            }
        } else {
            zskiplistNode *x = zsl_get_element_by_rank(zs->zsl, (unsigned long)(len - start));
            for (int i = start; x && i <= stop; i++, x = x->backward) {
                PyObject *item = zset_py_member_score(x->member, x->score, withscores);
                if (!item || PyList_Append(result, item) < 0) { Py_XDECREF(item); Py_DECREF(result); result = NULL; break; }
                Py_DECREF(item);
            }
        }
    }
    pthread_rwlock_unlock(&s->lock);
    return result;
}

PyObject *py_zrange(PyObject *self, PyObject *args, PyObject *kw) {
    return py_zrange_common(self, args, kw, 0);
}

PyObject *py_zrevrange(PyObject *self, PyObject *args, PyObject *kw) {
    return py_zrange_common(self, args, kw, 1);
}

static PyObject *py_zrank_common(PyObject *self, PyObject *args, int reverse) {
    (void)self;
    PyObject *handle, *key_obj, *member_obj;
    if (!PyArg_ParseTuple(args, "OOO", &handle, &key_obj, &member_obj)) return NULL;
    credish_store *s = credish_get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    sds member = pyobj_to_sds(member_obj);
    if (!member) return NULL;

    pthread_rwlock_wrlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    credishObject *o = db_lookup(db, key, keylen);
    PyObject *result = Py_None;
    Py_INCREF(result);
    if (o) {
        if (o->type != OBJ_ZSET) {
            pthread_rwlock_unlock(&s->lock);
            sds_free(member);
            Py_DECREF(result);
            PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
            return NULL;
        }
        zset *zs = (zset *)o->ptr;
        double *score = dict_fetch_value(zs->dict, member);
        if (score) {
            unsigned long rank = zsl_get_rank(zs->zsl, *score, member);
            Py_DECREF(result);
            result = reverse
                ? PyLong_FromUnsignedLong(zs->zsl->length - rank)
                : PyLong_FromUnsignedLong(rank - 1);
        }
    }
    pthread_rwlock_unlock(&s->lock);
    sds_free(member);
    return result;
}

PyObject *py_zrank(PyObject *self, PyObject *args) {
    return py_zrank_common(self, args, 0);
}

PyObject *py_zrevrank(PyObject *self, PyObject *args) {
    return py_zrank_common(self, args, 1);
}

PyObject *py_zscore(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj, *member_obj;
    if (!PyArg_ParseTuple(args, "OOO", &handle, &key_obj, &member_obj)) return NULL;
    credish_store *s = credish_get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    sds member = pyobj_to_sds(member_obj);
    if (!member) return NULL;

    pthread_rwlock_wrlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    credishObject *o = db_lookup(db, key, keylen);
    PyObject *result = Py_None;
    Py_INCREF(result);
    if (o) {
        if (o->type != OBJ_ZSET) {
            pthread_rwlock_unlock(&s->lock);
            sds_free(member);
            Py_DECREF(result);
            PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
            return NULL;
        }
        double *score = dict_fetch_value(((zset *)o->ptr)->dict, member);
        if (score) {
            Py_DECREF(result);
            result = PyFloat_FromDouble(*score);
        }
    }
    pthread_rwlock_unlock(&s->lock);
    sds_free(member);
    return result;
}

PyObject *py_zrem(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj, *members_list;
    if (!PyArg_ParseTuple(args, "OOO", &handle, &key_obj, &members_list)) return NULL;
    if (!PyList_Check(members_list)) { PyErr_SetString(PyExc_TypeError, "expected list"); return NULL; }
    credish_store *s = credish_get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    Py_ssize_t n = PyList_GET_SIZE(members_list);
    sds *members = calloc((size_t)n, sizeof(sds));
    int *removed_flags = calloc((size_t)n, sizeof(int));
    if ((n > 0) && (!members || !removed_flags)) {
        free(members); free(removed_flags);
        return PyErr_NoMemory();
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        members[i] = pyobj_to_sds(PyList_GET_ITEM(members_list, i));
        if (!members[i]) {
            for (Py_ssize_t j = 0; j < i; j++) sds_free(members[j]);
            free(members); free(removed_flags);
            return NULL;
        }
    }

    int removed = 0;
    pthread_rwlock_wrlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    credishObject *o = db_lookup_write(db, key, keylen);
    if (o) {
        if (o->type != OBJ_ZSET) {
            pthread_rwlock_unlock(&s->lock);
            for (Py_ssize_t i = 0; i < n; i++) sds_free(members[i]);
            free(members); free(removed_flags);
            PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
            return NULL;
        }
        zset *zs = (zset *)o->ptr;
        for (Py_ssize_t i = 0; i < n; i++) {
            removed_flags[i] = zset_delete_member(zs, members[i]);
            removed += removed_flags[i];
        }
    }
    pthread_rwlock_unlock(&s->lock);

    for (Py_ssize_t i = 0; i < n; i++) {
        if (removed_flags[i]) {
            const char *aof_argv[] = { key, members[i] };
            aof_append(s, "ZREM", 2, aof_argv);
        }
        sds_free(members[i]);
    }
    free(members); free(removed_flags);
    return PyLong_FromLong(removed);
}

PyObject *py_zcard(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj;
    if (!PyArg_ParseTuple(args, "OO", &handle, &key_obj)) return NULL;
    credish_store *s = credish_get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    pthread_rwlock_wrlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    credishObject *o = db_lookup(db, key, keylen);
    size_t sz = 0;
    if (o) {
        if (o->type != OBJ_ZSET) {
            pthread_rwlock_unlock(&s->lock);
            PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
            return NULL;
        }
        sz = ((zset *)o->ptr)->zsl->length;
    }
    pthread_rwlock_unlock(&s->lock);
    return PyLong_FromSsize_t((Py_ssize_t)sz);
}

PyObject *py_zrangebyscore(PyObject *self, PyObject *args, PyObject *kw) {
    (void)self;
    static char *kwlist[] = {"handle","key","min","max","withscores",NULL};
    PyObject *handle, *key_obj, *min_obj, *max_obj;
    int withscores = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOOO|b", kwlist,
            &handle, &key_obj, &min_obj, &max_obj, &withscores))
        return NULL;
    double min_score, max_score;
    if (!zset_score_from_py(min_obj, &min_score) || !zset_score_from_py(max_obj, &max_score))
        return NULL;
    credish_store *s = credish_get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;

    pthread_rwlock_wrlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    credishObject *o = db_lookup(db, key, keylen);
    if (!o) {
        pthread_rwlock_unlock(&s->lock);
        return PyList_New(0);
    }
    if (o->type != OBJ_ZSET) {
        pthread_rwlock_unlock(&s->lock);
        PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
        return NULL;
    }
    PyObject *result = PyList_New(0);
    if (result && min_score <= max_score) {
        zskiplistNode *x = ((zset *)o->ptr)->zsl->header->level[0].forward;
        while (x && x->score < min_score) x = x->level[0].forward;
        while (x && x->score <= max_score) {
            PyObject *item = zset_py_member_score(x->member, x->score, withscores);
            if (!item || PyList_Append(result, item) < 0) { Py_XDECREF(item); Py_DECREF(result); result = NULL; break; }
            Py_DECREF(item);
            x = x->level[0].forward;
        }
    }
    pthread_rwlock_unlock(&s->lock);
    return result;
}

PyObject *py_zincrby(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *handle, *key_obj, *amount_obj, *member_obj;
    if (!PyArg_ParseTuple(args, "OOOO", &handle, &key_obj, &amount_obj, &member_obj)) return NULL;
    double amount;
    if (!zset_score_from_py(amount_obj, &amount)) return NULL;
    credish_store *s = credish_get_store(handle); if (!s) return NULL;
    char *key; int keylen;
    if (!decode_key(key_obj, &key, &keylen)) return NULL;
    sds member = pyobj_to_sds(member_obj);
    if (!member) return NULL;

    double new_score;
    pthread_rwlock_wrlock(&s->lock);
    credish_db *db = credish_get_db(handle, s);
    credishObject *o = db_lookup_write(db, key, keylen);
    if (!o) {
        o = obj_create_zset();
        if (o) db_set(db, key, keylen, o, s);
    } else if (o->type != OBJ_ZSET) {
        pthread_rwlock_unlock(&s->lock);
        sds_free(member);
        PyErr_SetString(PyExc_TypeError, "WRONGTYPE");
        return NULL;
    }
    if (!o) {
        pthread_rwlock_unlock(&s->lock);
        sds_free(member);
        return PyErr_NoMemory();
    }
    zset *zs = (zset *)o->ptr;
    double *oldp = dict_fetch_value(zs->dict, member);
    new_score = (oldp ? *oldp : 0.0) + amount;
    if (isnan(new_score)) {
        pthread_rwlock_unlock(&s->lock);
        sds_free(member);
        PyErr_SetString(PyExc_ValueError, "score is not a valid float");
        return NULL;
    }
    int added = 0, changed = 0;
    zset_add(zs, member, new_score, 0, 0, 0, 0, &added, &changed);
    pthread_rwlock_unlock(&s->lock);

    char scorebuf[64];
    snprintf(scorebuf, sizeof(scorebuf), "%.17g", new_score);
    const char *aof_argv[] = { key, scorebuf, member };
    aof_append(s, "ZADD", 3, aof_argv);
    sds_free(member);
    return PyFloat_FromDouble(new_score);
}
