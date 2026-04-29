#ifndef CREDISH_OBJECT_H
#define CREDISH_OBJECT_H

#include <stdint.h>

/* Object type tags — match Redis type numbers */
#define OBJ_STRING  0
#define OBJ_LIST    1
#define OBJ_SET     2
#define OBJ_ZSET    3
#define OBJ_HASH    4

/* Forward declarations */
struct adlist;
struct dict;
struct zskiplist;

typedef struct credishObject {
    int   type;
    union {
        void           *ptr;   /* string (sds), or pointer to container */
        int64_t         ival;  /* small integer optimisation            */
    };
} credishObject;

credishObject *obj_create_string(const char *data, int len);
credishObject *obj_create_string_int(int64_t val);
credishObject *obj_create_list(void);
credishObject *obj_create_hash(void);
credishObject *obj_create_set(void);
credishObject *obj_create_zset(void);

void           obj_free(credishObject *o);

/* Helpers */
int   obj_is_string(const credishObject *o);
char *obj_string_ptr(const credishObject *o, int *len_out);

#endif /* CREDISH_OBJECT_H */
