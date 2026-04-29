#ifndef CREDISH_DICT_H
#define CREDISH_DICT_H

#include <stdint.h>
#include <stddef.h>

/* Hash table entry */
typedef struct dictEntry {
    void           *key;
    union {
        void       *val;
        int64_t     i64;
        double      f64;
    } v;
    struct dictEntry *next; /* chaining for collisions */
} dictEntry;

/* Per-table metadata */
typedef struct dictTable {
    dictEntry **buckets;
    size_t      size;       /* number of buckets (always power of 2) */
    size_t      used;       /* number of entries stored              */
    size_t      size_mask;  /* size - 1, for fast modulo             */
} dictTable;

/* Function table so dict is generic */
typedef struct dictType {
    uint64_t  (*hash)(const void *key);
    void     *(*key_dup)(void *key);
    void     *(*val_dup)(void *val);
    int       (*key_cmp)(const void *a, const void *b);
    void      (*key_free)(void *key);
    void      (*val_free)(void *val);
} dictType;

typedef struct dict {
    dictType  *type;
    dictTable  ht[2];   /* ht[1] used during incremental rehash */
    int        rehash_idx; /* -1 when not rehashing              */
    size_t     iterators;  /* number of active safe iterators    */
} dict;

typedef struct dictIterator {
    dict      *d;
    int        table;
    long       idx;
    dictEntry *entry;
    dictEntry *next_entry;
} dictIterator;

/* API */
dict        *dict_create(dictType *type);
void         dict_free(dict *d);
int          dict_add(dict *d, void *key, void *val);
int          dict_replace(dict *d, void *key, void *val);
dictEntry   *dict_find(dict *d, const void *key);
void        *dict_fetch_value(dict *d, const void *key);
int          dict_delete(dict *d, const void *key);
size_t       dict_size(const dict *d);

dictIterator *dict_iter_new(dict *d);
dictEntry    *dict_iter_next(dictIterator *it);
void          dict_iter_free(dictIterator *it);

/* Common hash functions */
uint64_t dict_hash_sds(const void *key);
int      dict_cmp_sds(const void *a, const void *b);

#endif /* CREDISH_DICT_H */
