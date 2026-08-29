#ifndef CREDISH_PY_HELPERS_H
#define CREDISH_PY_HELPERS_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdint.h>
#include "db.h"
#include "sds.h"
#include "object.h"

#define CAPSULE_NAME "credish._credish.store"

extern int g_closed_sentinel;

credish_store *credish_get_store(PyObject *handle);
int get_db_id(PyObject *handle);
credish_db *credish_get_db(PyObject *handle, credish_store *store);
int decode_key(PyObject *obj, char **out, int *out_len);
sds pyobj_to_sds(PyObject *object);
int64_t now_ms_mod(void);

#endif /* CREDISH_PY_HELPERS_H */

