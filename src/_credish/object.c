#include "object.h"
#include "bufpool.h"
#include "sds.h"
#include "dict.h"
#include "adlist.h"
#include "skiplist.h"
#include <stdlib.h>
#include <stdio.h>

credishObject *obj_create_string(const char *data, int len) {
    credishObject *o = bufpool_alloc(sizeof(*o));
    if (!o) return NULL;
    o->type = OBJ_STRING;
    o->ptr  = sds_newlen(data, (size_t)len);
    return o;
}

credishObject *obj_steal_string(sds s) {
    credishObject *o = bufpool_alloc(sizeof(*o));
    if (!o) return NULL;
    o->type = OBJ_STRING;
    o->ptr  = s;
    return o;
}

credishObject *obj_create_string_int(int64_t val) {
    credishObject *o = bufpool_alloc(sizeof(*o));
    if (!o) return NULL;
    o->type = OBJ_STRING;
    char buf[22];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)val);
    o->ptr = sds_newlen(buf, (size_t)n);
    return o;
}

credishObject *obj_create_list(void) {
    credishObject *o = bufpool_alloc(sizeof(*o));
    if (!o) return NULL;
    o->type = OBJ_LIST;
    o->ptr  = adlist_create();
    return o;
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
    credishObject *o = bufpool_alloc(sizeof(*o));
    if (!o) return NULL;
    o->type = OBJ_HASH;
    o->ptr  = dict_create(&hash_dict_type);
    return o;
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
    credishObject *o = bufpool_alloc(sizeof(*o));
    if (!o) return NULL;
    o->type = OBJ_SET;
    o->ptr  = dict_create(&set_dict_type);
    return o;
}

typedef struct zset {
    dict       *dict;    /* member -> score */
    zskiplist  *zsl;
} zset;

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
    credishObject *o = bufpool_alloc(sizeof(*o));
    if (!o) return NULL;
    zset *zs = bufpool_alloc(sizeof(zset));
    if (!zs) { bufpool_free(o, sizeof(*o)); return NULL; }
    zs->dict = dict_create(&zset_dict_type);
    zs->zsl  = zsl_create();
    o->type  = OBJ_ZSET;
    o->ptr   = zs;
    return o;
}

void obj_free(credishObject *o) {
    if (!o) return;
    switch (o->type) {
    case OBJ_STRING:
        sds_free((sds)o->ptr);
        break;
    case OBJ_LIST:
        adlist_free((struct adlist *)o->ptr, (void (*)(void *))sds_free);
        break;
    case OBJ_HASH:
        dict_free((dict *)o->ptr);
        break;
    case OBJ_SET:
        dict_free((dict *)o->ptr);
        break;
    case OBJ_ZSET: {
        zset *zs = (zset *)o->ptr;
        dict_free(zs->dict);
        zsl_free(zs->zsl);
        bufpool_free(zs, sizeof(zset));
        break;
    }
    }
    bufpool_free(o, sizeof(*o));
}

int obj_is_string(const credishObject *o) {
    return o && o->type == OBJ_STRING;
}

char *obj_string_ptr(const credishObject *o, int *len_out) {
    if (!o || o->type != OBJ_STRING) return NULL;
    sds s = (sds)o->ptr;
    if (len_out) *len_out = (int)SDS_LEN(s);
    return s;
}
