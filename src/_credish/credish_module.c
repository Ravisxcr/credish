/*
 * credish_module.c — Python C extension entry point.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "bufpool.h"
#include "db.h"
#include "dict.h"
#include "server.h"
#include "persistence/rdb.h"
#include "py_helpers.h"
#include "py_string.h"
#include "py_list.h"
#include "py_key.h"
#include "py_hash.h"
#include "py_zset.h"
#include "platform.h"
#include <string.h>
#include <stdlib.h>

static void store_capsule_destructor(PyObject *capsule)
{
    credish_store *store = PyCapsule_GetPointer(capsule, CAPSULE_NAME);
    if (store && store != (credish_store *)&g_closed_sentinel)
        store_close(store);
}

// Open / Close

static PyObject *py_open(PyObject *self, PyObject *args, PyObject *kwargs)
{
    (void)self;
    static char *kwlist[] = {"data_dir", "persistence", "save_interval", "aof_fsync", "db", "decode_responses", NULL};
    const char *data_dir = ".";
    const char *persistence = "hybrid";
    int save_interval = 300;
    const char *aof_fsync = "everysec";
    int db_id = 0;
    int decode_responses = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|ssisip", kwlist,
                                     &data_dir, &persistence, &save_interval, &aof_fsync, &db_id, &decode_responses))
        return NULL;

    credish_config server_config = {0};
    strncpy(server_config.data_dir, data_dir, sizeof(server_config.data_dir) - 1);
    server_config.persist_mode = parse_persist_mode(persistence);
    server_config.save_interval = save_interval;
    server_config.aof_fsync = parse_aof_fsync(aof_fsync);
    server_config.decode_responses = decode_responses ? 1 : 0;

    credish_store *store = store_open(&server_config);
    if (!store)
        return PyErr_NoMemory();

    return PyCapsule_New(store, CAPSULE_NAME, store_capsule_destructor);
}

static PyObject *py_close(PyObject *self, PyObject *args)
{
    (void)self;
    PyObject *handle;

    if (!PyArg_ParseTuple(args, "O", &handle))
        return NULL;

    credish_store *store = credish_get_store(handle);
    if (!store)
        return NULL;

    Py_BEGIN_ALLOW_THREADS
        store_close(store);
    Py_END_ALLOW_THREADS

        /* Invalidate the capsule so the destructor doesn't double-free */
        PyCapsule_SetDestructor(handle, NULL);
    PyCapsule_SetPointer(handle, &g_closed_sentinel);

    Py_RETURN_NONE;
}

// ping / flushdb / dbsize / select

static PyObject *py_ping(PyObject *self, PyObject *args)
{
    (void)self;
    PyObject *handle;

    if (!PyArg_ParseTuple(args, "O", &handle))
        return NULL;

    return PyUnicode_FromString("PONG");
}

static PyObject *py_flushdb(PyObject *self, PyObject *args)
{
    (void)self;
    PyObject *handle;

    if (!PyArg_ParseTuple(args, "O", &handle))
        return NULL;
    credish_store *store = credish_get_store(handle);
    if (!store)
        return NULL;

    Py_BEGIN_ALLOW_THREADS
        credish_rwlock_wrlock(&store->lock);

    for (int i = 0; i < CREDISH_DB_COUNT; i++)
    {
        dict_free(store->dbs[i].keys);
        dict_free(store->dbs[i].expires);
        /* dict_create requires dict_type — re-init deferred; just wipe used */
    }

    credish_rwlock_wrunlock(&store->lock);
    Py_END_ALLOW_THREADS

    Py_RETURN_TRUE;
}

static PyObject *py_dbsize(PyObject *self, PyObject *args)
{
    (void)self;
    PyObject *handle;
    int db_id = -1;

    if (!PyArg_ParseTuple(args, "O|i", &handle, &db_id))
        return NULL;

    credish_store *store = credish_get_store(handle);
    if (!store)
        return NULL;

    if (db_id < 0)
        db_id = get_db_id(handle);

    credish_db *database = store_select_db(store, db_id);

    if (!database)
        return PyLong_FromLong(0);

    credish_rwlock_rdlock(&store->lock);
    size_t size = dict_size(database->keys);
    credish_rwlock_rdunlock(&store->lock);

    return PyLong_FromSsize_t((Py_ssize_t)size);
}

static PyObject *py_save(PyObject *self, PyObject *args)
{
    (void)self;
    PyObject *handle;
    int is_rdb_saved;

    if (!PyArg_ParseTuple(args, "O", &handle))
        return NULL;

    credish_store *store = credish_get_store(handle);

    if (!store)
        return NULL;

    Py_BEGIN_ALLOW_THREADS
    credish_rwlock_rdlock(&store->lock);

    is_rdb_saved = rdb_save(store);

    credish_rwlock_rdunlock(&store->lock);
    Py_END_ALLOW_THREADS

    return rc == 0 ? Py_True : Py_False;
}

static PyObject *py_bgsave(PyObject *self, PyObject *args)
{
    (void)self;
    PyObject *handle;

    if (!PyArg_ParseTuple(args, "O", &handle))
        return NULL;

    credish_store *store = credish_get_store(handle);
    if (!store)
        return NULL;
    rdb_bgsave(store);

    Py_RETURN_TRUE;
}

static PyObject *py_select(PyObject *self, PyObject *args)
{
    (void)self;
    PyObject *handle;
    int db_id;

    if (!PyArg_ParseTuple(args, "Oi", &handle, &db_id))
        return NULL;

    if (db_id < 0 || db_id >= CREDISH_DB_COUNT)
    {
        PyErr_SetString(PyExc_ValueError, "db index out of range");
        return NULL;
    }

    Py_RETURN_TRUE;
}

// Module method table

static PyMethodDef credish_methods[] = {
    {"open", (PyCFunction)(void (*)(void))py_open, METH_VARARGS | METH_KEYWORDS, NULL},
    {"close", py_close, METH_VARARGS, NULL},
    {"ping", py_ping, METH_VARARGS, NULL},
    {"flushdb", py_flushdb, METH_VARARGS, NULL},
    {"dbsize", py_dbsize, METH_VARARGS, NULL},
    {"save", py_save, METH_VARARGS, NULL},
    {"bgsave", py_bgsave, METH_VARARGS, NULL},
    {"select", py_select, METH_VARARGS, NULL},
    {"get", py_get, METH_VARARGS, NULL},
    {"get_encoding", py_get_encoding, METH_VARARGS, NULL},
    {"set", (PyCFunction)(void (*)(void))py_set, METH_VARARGS | METH_KEYWORDS, NULL},
    {"delete", py_delete, METH_VARARGS, NULL},
    {"exists", py_exists, METH_VARARGS, NULL},
    {"expire", py_expire, METH_VARARGS, NULL},
    {"pexpire", py_pexpire, METH_VARARGS, NULL},
    {"persist", py_persist, METH_VARARGS, NULL},
    {"ttl", py_ttl, METH_VARARGS, NULL},
    {"pttl", py_pttl, METH_VARARGS, NULL},
    {"type_", py_type, METH_VARARGS, NULL},
    {"incrby", py_incrby, METH_VARARGS, NULL},
    {"lpush", py_lpush, METH_VARARGS, NULL},
    {"rpush", py_rpush, METH_VARARGS, NULL},
    {"lrange", py_lrange, METH_VARARGS, NULL},
    {"llen", py_llen, METH_VARARGS, NULL},
    {"zadd", (PyCFunction)(void (*)(void))py_zadd, METH_VARARGS | METH_KEYWORDS, NULL},
    {"zrange", (PyCFunction)(void (*)(void))py_zrange, METH_VARARGS | METH_KEYWORDS, NULL},
    {"zrevrange", (PyCFunction)(void (*)(void))py_zrevrange, METH_VARARGS | METH_KEYWORDS, NULL},
    {"zrank", py_zrank, METH_VARARGS, NULL},
    {"zrevrank", py_zrevrank, METH_VARARGS, NULL},
    {"zscore", py_zscore, METH_VARARGS, NULL},
    {"zrem", py_zrem, METH_VARARGS, NULL},
    {"zcard", py_zcard, METH_VARARGS, NULL},
    {"zrangebyscore", (PyCFunction)(void (*)(void))py_zrangebyscore, METH_VARARGS | METH_KEYWORDS, NULL},
    {"zincrby", py_zincrby, METH_VARARGS, NULL},
    {"hset", (PyCFunction)(void (*)(void))py_hset, METH_VARARGS | METH_KEYWORDS, NULL},
    {"hget", (PyCFunction)(void (*)(void))py_hget, METH_VARARGS | METH_KEYWORDS, NULL},
    {"hmset", py_hmset, METH_VARARGS, NULL},
    {"hmget", (PyCFunction)(void (*)(void))py_hmget, METH_VARARGS | METH_KEYWORDS, NULL},
    {"hdel", py_hdel, METH_VARARGS, NULL},
    {"hexists", py_hexists, METH_VARARGS, NULL},
    {"hgetall", (PyCFunction)(void (*)(void))py_hgetall, METH_VARARGS | METH_KEYWORDS, NULL},
    {"hkeys", (PyCFunction)(void (*)(void))py_hkeys, METH_VARARGS | METH_KEYWORDS, NULL},
    {"hvals", (PyCFunction)(void (*)(void))py_hvals, METH_VARARGS | METH_KEYWORDS, NULL},
    {"hlen", py_hlen, METH_VARARGS, NULL},
    {"hincrby", py_hincrby, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL}};

static struct PyModuleDef credish_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_credish",
    .m_doc = "Credish C extension - Redis-compatible in-process cache",
    .m_size = -1,
    .m_methods = credish_methods,
};

PyMODINIT_FUNC PyInit__credish(void)
{
    bufpool_init();
    return PyModule_Create(&credish_module);
}
