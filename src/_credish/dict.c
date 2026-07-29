#include "dict.h"
#include "bufpool.h"
#include "sds.h"
#include <stdlib.h>
#include <string.h>

#define DICT_INITIAL_SIZE 4
#define DICT_LOAD_FACTOR  1

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int dict_expand(dict *dictionary, size_t size);
static void dict_rehash_step(dict *dictionary);
static dictEntry *dict_find_raw(dict *dictionary, const void *key, int table);

static size_t next_power(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

/* MurmurHash-inspired 64-bit mix */
uint64_t dict_hash_sds(const void *key) {
    const sds str = (const sds)key;
    size_t len  = SDS_LEN(str);
    const uint8_t *data = (const uint8_t *)str;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

int dict_cmp_sds(const void *a, const void *b) {
    return sds_cmp((sds)a, (sds)b);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

dict *dict_create(dictType *type) {
    dict *dictionary = bufpool_alloc(sizeof(*dictionary));
    if (!dictionary) return NULL;
    memset(dictionary, 0, sizeof(*dictionary));
    dictionary->type       = type;
    dictionary->rehash_idx = -1;
    return dictionary;
}

static void dict_table_free(dict *dictionary, int table) {
    dictTable *bucket_table = &dictionary->ht[table];
    if (!bucket_table->buckets) return;
    for (size_t i = 0; i < bucket_table->size; i++) {
        dictEntry *entry = bucket_table->buckets[i];
        while (entry) {
            dictEntry *next = entry->next;
            if (dictionary->type->key_free) dictionary->type->key_free(entry->key);
            if (dictionary->type->val_free) dictionary->type->val_free(entry->v.val);
            bufpool_free(entry, sizeof(*entry));
            entry = next;
        }
    }
    free(bucket_table->buckets);
    memset(bucket_table, 0, sizeof(*bucket_table));
}

void dict_free(dict *dictionary) {
    dict_table_free(dictionary, 0);
    dict_table_free(dictionary, 1);
    bufpool_free(dictionary, sizeof(*dictionary));
}

size_t dict_size(const dict *dictionary) {
    return dictionary->ht[0].used + dictionary->ht[1].used;
}

/* ------------------------------------------------------------------ */
/* Rehashing                                                           */
/* ------------------------------------------------------------------ */

static int dict_expand(dict *dictionary, size_t size) {
    if (dictionary->rehash_idx != -1) return -1; /* already rehashing */
    size_t real = next_power(size < DICT_INITIAL_SIZE ? DICT_INITIAL_SIZE : size);
    int is_rehash = (dictionary->ht[0].buckets != NULL);
    dictTable *bucket_table = &dictionary->ht[is_rehash ? 1 : 0];
    bucket_table->size      = real;
    bucket_table->size_mask = real - 1;
    bucket_table->used      = 0;
    bucket_table->buckets   = calloc(real, sizeof(dictEntry *));
    if (!bucket_table->buckets) return -1;
    if (is_rehash) dictionary->rehash_idx = 0; /* start incremental rehash */
    return 0;
}

static void dict_rehash_step(dict *dictionary) {
    if (dictionary->rehash_idx == -1 || dictionary->iterators) return;
    dictTable *src = &dictionary->ht[0];
    dictTable *dst = &dictionary->ht[1];

    /* Move one non-empty bucket */
    while ((size_t)dictionary->rehash_idx < src->size && !src->buckets[dictionary->rehash_idx])
        dictionary->rehash_idx++;
    if ((size_t)dictionary->rehash_idx >= src->size) goto done;

    dictEntry *entry = src->buckets[dictionary->rehash_idx];
    src->buckets[dictionary->rehash_idx] = NULL;
    while (entry) {
        dictEntry *next = entry->next;
        size_t idx = dictionary->type->hash(entry->key) & dst->size_mask;
        entry->next = dst->buckets[idx];
        dst->buckets[idx] = entry;
        src->used--;
        dst->used++;
        entry = next;
    }
    dictionary->rehash_idx++;

done:
    if (src->used == 0) {
        free(src->buckets);
        *src = *dst;
        memset(dst, 0, sizeof(*dst));
        dictionary->rehash_idx = -1;
    }
}

static void dict_maybe_expand(dict *dictionary) {
    if (dictionary->rehash_idx != -1) return;
    if (!dictionary->ht[0].buckets) {
        dict_expand(dictionary, DICT_INITIAL_SIZE);
        return;
    }
    if (dictionary->ht[0].used >= dictionary->ht[0].size * DICT_LOAD_FACTOR)
        dict_expand(dictionary, dictionary->ht[0].used * 2);
}

/* ------------------------------------------------------------------ */
/* Core operations                                                     */
/* ------------------------------------------------------------------ */

static dictEntry *dict_find_raw(dict *dictionary, const void *key, int table) {
    dictTable *bucket_table = &dictionary->ht[table];
    if (!bucket_table->size) return NULL;
    size_t idx = dictionary->type->hash(key) & bucket_table->size_mask;
    dictEntry *entry = bucket_table->buckets[idx];
    while (entry) {
        if (dictionary->type->key_cmp(entry->key, key) == 0) return entry;
        entry = entry->next;
    }
    return NULL;
}

dictEntry *dict_find(dict *dictionary, const void *key) {
    if (dictionary->rehash_idx != -1) dict_rehash_step(dictionary);
    dictEntry *entry = dict_find_raw(dictionary, key, 0);
    if (!entry && dictionary->rehash_idx != -1) entry = dict_find_raw(dictionary, key, 1);
    return entry;
}

void *dict_fetch_value(dict *dictionary, const void *key) {
    dictEntry *entry = dict_find(dictionary, key);
    return entry ? entry->v.val : NULL;
}

int dict_add(dict *dictionary, void *key, void *val) {
    dict_maybe_expand(dictionary);
    if (dictionary->rehash_idx != -1) dict_rehash_step(dictionary);

    if (dict_find(dictionary, key)) return -1; /* key already exists */

    dictTable *bucket_table = (dictionary->rehash_idx != -1) ? &dictionary->ht[1] : &dictionary->ht[0];
    size_t idx       = dictionary->type->hash(key) & bucket_table->size_mask;
    dictEntry *entry = bufpool_alloc(sizeof(*entry));
    if (!entry) return -1;
    entry->key = dictionary->type->key_dup ? dictionary->type->key_dup(key) : key;
    entry->v.val = dictionary->type->val_dup ? dictionary->type->val_dup(val) : val;
    entry->next  = bucket_table->buckets[idx];
    bucket_table->buckets[idx] = entry;
    bucket_table->used++;
    return 0;
}

int dict_replace(dict *dictionary, void *key, void *val) {
    dictEntry *entry = dict_find(dictionary, key);
    if (entry) {
        if (dictionary->type->val_free) dictionary->type->val_free(entry->v.val);
        entry->v.val = dictionary->type->val_dup ? dictionary->type->val_dup(val) : val;
        return 0; /* updated */
    }
    return dict_add(dictionary, key, val); /* inserted */
}

int dict_delete(dict *dictionary, const void *key) {
    for (int table = 0; table <= 1; table++) {
        dictTable *bucket_table = &dictionary->ht[table];
        if (!bucket_table->size) continue;
        size_t idx = dictionary->type->hash(key) & bucket_table->size_mask;
        dictEntry *entry = bucket_table->buckets[idx];
        dictEntry *prev  = NULL;
        while (entry) {
            if (dictionary->type->key_cmp(entry->key, key) == 0) {
                if (prev) prev->next = entry->next;
                else       bucket_table->buckets[idx] = entry->next;
                if (dictionary->type->key_free) dictionary->type->key_free(entry->key);
                if (dictionary->type->val_free) dictionary->type->val_free(entry->v.val);
                bufpool_free(entry, sizeof(*entry));
                bucket_table->used--;
                return 0;
            }
            prev  = entry;
            entry = entry->next;
        }
        if (dictionary->rehash_idx == -1) break;
    }
    return -1; /* not found */
}

/* ------------------------------------------------------------------ */
/* Iterator                                                            */
/* ------------------------------------------------------------------ */

dictIterator *dict_iter_new(dict *dictionary) {
    dictIterator *it = bufpool_alloc(sizeof(*it));
    if (!it) return NULL;
    it->dictionary = dictionary;
    it->table      = 0;
    it->idx        = -1;
    it->entry      = NULL;
    it->next_entry = NULL;
    dictionary->iterators++;
    return it;
}

dictEntry *dict_iter_next(dictIterator *it) {
    while (1) {
        if (it->entry) {
            it->entry = it->next_entry;
        } else {
            dictTable *bucket_table = &it->dictionary->ht[it->table];
            it->idx++;
            if ((size_t)it->idx >= bucket_table->size) {
                if (it->table == 0 && it->dictionary->rehash_idx != -1) {
                    it->table = 1;
                    it->idx   = 0;
                    bucket_table = &it->dictionary->ht[1];
                } else {
                    return NULL;
                }
            }
            it->entry = bucket_table->buckets[it->idx];
        }
        if (it->entry) {
            it->next_entry = it->entry->next;
            return it->entry;
        }
    }
}

void dict_iter_free(dictIterator *it) {
    it->dictionary->iterators--;
    bufpool_free(it, sizeof(*it));
}
