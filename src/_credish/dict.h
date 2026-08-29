#ifndef CREDISH_DICT_H
#define CREDISH_DICT_H

#include <stdint.h>
#include <stddef.h>

/* Hash table entry */
typedef struct dict_entry
{
    void *key;
    union
    {
        void *val;
        int64_t i64;
        double f64;
    } v;
    struct dict_entry *next; /* chaining for collisions */
} dict_entry;

/* Per-table metadata */
typedef struct dict_table
{
    dict_entry **buckets;
    size_t size;      /* number of buckets (always power of 2) */
    size_t used;      /* number of entries stored              */
    size_t size_mask; /* size - 1, for fast modulo             */
} dict_table;

/* Function table so dict is generic */
typedef struct dict_type
{
    uint64_t (*hash)(const void *key);
    void *(*key_dup)(void *key);
    void *(*val_dup)(void *val);
    int (*key_cmp)(const void *a, const void *b);
    void (*key_free)(void *key);
    void (*val_free)(void *val);
} dict_type;

typedef struct dict
{
    dict_type *type;
    dict_table ht[2]; /* ht[1] used during incremental rehash */
    int rehash_idx;   /* -1 when not rehashing              */
    size_t iterators; /* number of active safe iterators    */
} dict;

typedef struct dict_iterator
{
    dict *dictionary;
    int table;
    long idx;
    dict_entry *entry;
    dict_entry *next_entry;
} dict_iterator;

/* API */
dict *dict_create(dict_type *type);
void dict_free(dict *dictionary);
int dict_add(dict *dictionary, void *key, void *val);
int dict_replace(dict *dictionary, void *key, void *val);
dict_entry *dict_find(dict *dictionary, const void *key);
void *dict_fetch_value(dict *dictionary, const void *key);
int dict_delete(dict *dictionary, const void *key);
size_t dict_size(const dict *dictionary);

dict_iterator *dict_iter_new(dict *dictionary);
dict_entry *dict_iter_next(dict_iterator *it);
void dict_iter_free(dict_iterator *it);

/* Common hash functions */
uint64_t dict_hash_sds(const void *key);
int dict_cmp_sds(const void *a, const void *b);

#endif /* CREDISH_DICT_H */
