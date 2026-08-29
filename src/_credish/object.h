#ifndef CREDISH_OBJECT_H
#define CREDISH_OBJECT_H

#include <stdint.h>
#include "sds.h"

/* Object type tags — match Redis type numbers */
#define OBJ_STRING 0
#define OBJ_LIST 1
#define OBJ_SET 2
#define OBJ_ZSET 3
#define OBJ_HASH 4

#define OBJ_ENCODING_RAW 0
#define OBJ_ENCODING_JSON 1
#define OBJ_ENCODING_STR 2
#define OBJ_ENCODING_INT 3
#define OBJ_ENCODING_FLOAT 4

/* Forward declarations */
struct adlist;
struct dict;
struct zskiplist;

typedef struct zset
{
    struct dict *dict;     /* member -> score */
    struct zskiplist *zsl; /* score/member ordered view */
} zset;

typedef struct credishObject
{
    int type;
    int encoding;
    union
    {
        void *ptr;    /* string (sds), or pointer to container */
        int64_t ival; /* small integer optimisation            */
    };
} credishObject;

credishObject *obj_create_string(const char *data, int len);
credishObject *obj_create_string_encoded(const char *data, int len, int encoding);
credishObject *obj_steal_string(sds str); /* takes ownership of str — no copy */
credishObject *obj_steal_string_encoded(sds str, int encoding);
credishObject *obj_create_string_int(int64_t val);
credishObject *obj_create_list(void);
credishObject *obj_create_hash(void);
credishObject *obj_create_set(void);
credishObject *obj_create_zset(void);

void obj_free(credishObject *obj);

int obj_is_string(const credishObject *obj);
char *obj_string_ptr(const credishObject *obj, int *len_out);

#endif /* CREDISH_OBJECT_H */
