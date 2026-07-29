#include "object.h"
#include "bufpool.h"
#include "sds.h"
#include "dict.h"
#include "adlist.h"
#include "skiplist.h"
#include <stdlib.h>
#include <stdio.h>

credishObject *obj_create_string_encoded(const char *data, int len, int encoding) {
    credishObject *obj = bufpool_alloc(sizeof(*obj));
    if (!obj) return NULL;
    obj->type = OBJ_STRING;
    obj->encoding = encoding;
    obj->ptr  = sds_newlen(data, (size_t)len);
    return obj;
}

credishObject *obj_create_string(const char *data, int len) {
    return obj_create_string_encoded(data, len, OBJ_ENCODING_RAW);
}

credishObject *obj_steal_string_encoded(sds str, int encoding) {
    credishObject *obj = bufpool_alloc(sizeof(*obj));
    if (!obj) return NULL;
    obj->type = OBJ_STRING;
    obj->encoding = encoding;
    obj->ptr  = str;
    return obj;
}

credishObject *obj_steal_string(sds str) {
    return obj_steal_string_encoded(str, OBJ_ENCODING_RAW);
}

credishObject *obj_create_string_int(int64_t val) {
    credishObject *obj = bufpool_alloc(sizeof(*obj));
    if (!obj) return NULL;
    obj->type = OBJ_STRING;
    obj->encoding = OBJ_ENCODING_RAW;
    char buf[22];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)val);
    obj->ptr = sds_newlen(buf, (size_t)n);
    return obj;
}

credishObject *obj_create_list(void) {
    credishObject *obj = bufpool_alloc(sizeof(*obj));
    if (!obj) return NULL;
    obj->type = OBJ_LIST;
    obj->encoding = OBJ_ENCODING_RAW;
    obj->ptr  = adlist_create();
    return obj;
}

/* dictType for hash objects: SDS keys and SDS values */
static dictType hash_dict_type = {
    .hash     = dict_hash_sds,
    .key_dup  = (void *(*)(void *))sds_dup,
    .val_dup  = (void *(*)(void *))sds_dup,
    .key_cmp  = dict_cmp_sds,
    .key_free = (void (*)(void *))sds_free,
    .val_free = (void (*)(void *))sds_free,
};

credishObject *obj_create_hash(void) {
    credishObject *obj = bufpool_alloc(sizeof(*obj));
    if (!obj) return NULL;
    obj->type = OBJ_HASH;
    obj->encoding = OBJ_ENCODING_RAW;
    obj->ptr  = dict_create(&hash_dict_type);
    return obj;
}

/* dictType for sets: SDS keys, NULL values */
static dictType set_dict_type = {
    .hash     = dict_hash_sds,
    .key_dup  = (void *(*)(void *))sds_dup,
    .val_dup  = NULL,
    .key_cmp  = dict_cmp_sds,
    .key_free = (void (*)(void *))sds_free,
    .val_free = NULL,
};

credishObject *obj_create_set(void) {
    credishObject *obj = bufpool_alloc(sizeof(*obj));
    if (!obj) return NULL;
    obj->type = OBJ_SET;
    obj->encoding = OBJ_ENCODING_RAW;
    obj->ptr  = dict_create(&set_dict_type);
    return obj;
}

/* dictType for zset score dict: SDS keys, double* values */
static void zset_val_free(void *v) { bufpool_free(v, sizeof(double)); }
static void *zset_val_dup(void *v) {
    double *d = bufpool_alloc(sizeof(double));
    if (d) *d = *(double *)v;
    return d;
}
static dictType zset_dict_type = {
    .hash     = dict_hash_sds,
    .key_dup  = (void *(*)(void *))sds_dup,
    .val_dup  = zset_val_dup,
    .key_cmp  = dict_cmp_sds,
    .key_free = (void (*)(void *))sds_free,
    .val_free = zset_val_free,
};

credishObject *obj_create_zset(void) {
    credishObject *obj = bufpool_alloc(sizeof(*obj));
    if (!obj) return NULL;
    zset *zs = bufpool_alloc(sizeof(zset));
    if (!zs) { bufpool_free(obj, sizeof(*obj)); return NULL; }
    zs->dict = dict_create(&zset_dict_type);
    zs->zsl  = zsl_create();
    obj->type  = OBJ_ZSET;
    obj->encoding = OBJ_ENCODING_RAW;
    obj->ptr   = zs;
    return obj;
}

void obj_free(credishObject *obj) {
    if (!obj) return;
    switch (obj->type) {
    case OBJ_STRING:
        sds_free((sds)obj->ptr);
        break;
    case OBJ_LIST:
        adlist_free((struct adlist *)obj->ptr, (void (*)(void *))sds_free);
        break;
    case OBJ_HASH:
        dict_free((dict *)obj->ptr);
        break;
    case OBJ_SET:
        dict_free((dict *)obj->ptr);
        break;
    case OBJ_ZSET: {
        zset *zs = (zset *)obj->ptr;
        dict_free(zs->dict);
        zsl_free(zs->zsl);
        bufpool_free(zs, sizeof(zset));
        break;
    }
    }
    bufpool_free(obj, sizeof(*obj));
}

int obj_is_string(const credishObject *obj) {
    return obj && obj->type == OBJ_STRING;
}

char *obj_string_ptr(const credishObject *obj, int *len_out) {
    if (!obj || obj->type != OBJ_STRING) return NULL;
    sds str = (sds)obj->ptr;
    if (len_out) *len_out = (int)SDS_LEN(str);
    return str;
}
