#include "dict.h"
#include "sds.h"
#include <stdlib.h>
#include <string.h>

#define DICT_INITIAL_SIZE 4
#define DICT_LOAD_FACTOR  1

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int dict_expand(dict *d, size_t size);
static void dict_rehash_step(dict *d);
static dictEntry *dict_find_raw(dict *d, const void *key, int table);

static size_t next_power(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

/* MurmurHash-inspired 64-bit mix */
uint64_t dict_hash_sds(const void *key) {
    const sds s = (const sds)key;
    size_t len  = SDS_LEN(s);
    const uint8_t *data = (const uint8_t *)s;
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
    dict *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->type      = type;
    d->rehash_idx = -1;
    return d;
}

static void dict_table_free(dict *d, int table) {
    dictTable *t = &d->ht[table];
    if (!t->buckets) return;
    for (size_t i = 0; i < t->size; i++) {
        dictEntry *e = t->buckets[i];
        while (e) {
            dictEntry *next = e->next;
            if (d->type->key_free) d->type->key_free(e->key);
            if (d->type->val_free) d->type->val_free(e->v.val);
            free(e);
            e = next;
        }
    }
    free(t->buckets);
    memset(t, 0, sizeof(*t));
}

void dict_free(dict *d) {
    dict_table_free(d, 0);
    dict_table_free(d, 1);
    free(d);
}

size_t dict_size(const dict *d) {
    return d->ht[0].used + d->ht[1].used;
}

/* ------------------------------------------------------------------ */
/* Rehashing                                                           */
/* ------------------------------------------------------------------ */

static int dict_expand(dict *d, size_t size) {
    if (d->rehash_idx != -1) return -1; /* already rehashing */
    size_t real = next_power(size < DICT_INITIAL_SIZE ? DICT_INITIAL_SIZE : size);
    int is_rehash = (d->ht[0].buckets != NULL);
    dictTable *t = &d->ht[is_rehash ? 1 : 0];
    t->size      = real;
    t->size_mask = real - 1;
    t->used      = 0;
    t->buckets   = calloc(real, sizeof(dictEntry *));
    if (!t->buckets) return -1;
    if (is_rehash) d->rehash_idx = 0; /* start incremental rehash */
    return 0;
}

static void dict_rehash_step(dict *d) {
    if (d->rehash_idx == -1 || d->iterators) return;
    dictTable *src = &d->ht[0];
    dictTable *dst = &d->ht[1];

    /* Move one non-empty bucket */
    while ((size_t)d->rehash_idx < src->size && !src->buckets[d->rehash_idx])
        d->rehash_idx++;
    if ((size_t)d->rehash_idx >= src->size) goto done;

    dictEntry *e = src->buckets[d->rehash_idx];
    src->buckets[d->rehash_idx] = NULL;
    while (e) {
        dictEntry *next = e->next;
        size_t idx = d->type->hash(e->key) & dst->size_mask;
        e->next = dst->buckets[idx];
        dst->buckets[idx] = e;
        src->used--;
        dst->used++;
        e = next;
    }
    d->rehash_idx++;

done:
    if (src->used == 0) {
        free(src->buckets);
        *src = *dst;
        memset(dst, 0, sizeof(*dst));
        d->rehash_idx = -1;
    }
}

static void dict_maybe_expand(dict *d) {
    if (d->rehash_idx != -1) return;
    if (!d->ht[0].buckets) {
        dict_expand(d, DICT_INITIAL_SIZE);
        return;
    }
    if (d->ht[0].used >= d->ht[0].size * DICT_LOAD_FACTOR)
        dict_expand(d, d->ht[0].used * 2);
}

/* ------------------------------------------------------------------ */
/* Core operations                                                     */
/* ------------------------------------------------------------------ */

static dictEntry *dict_find_raw(dict *d, const void *key, int table) {
    dictTable *t = &d->ht[table];
    if (!t->size) return NULL;
    size_t idx = d->type->hash(key) & t->size_mask;
    dictEntry *e = t->buckets[idx];
    while (e) {
        if (d->type->key_cmp(e->key, key) == 0) return e;
        e = e->next;
    }
    return NULL;
}

dictEntry *dict_find(dict *d, const void *key) {
    if (d->rehash_idx != -1) dict_rehash_step(d);
    dictEntry *e = dict_find_raw(d, key, 0);
    if (!e && d->rehash_idx != -1) e = dict_find_raw(d, key, 1);
    return e;
}

void *dict_fetch_value(dict *d, const void *key) {
    dictEntry *e = dict_find(d, key);
    return e ? e->v.val : NULL;
}

int dict_add(dict *d, void *key, void *val) {
    dict_maybe_expand(d);
    if (d->rehash_idx != -1) dict_rehash_step(d);

    if (dict_find(d, key)) return -1; /* key already exists */

    dictTable *t = (d->rehash_idx != -1) ? &d->ht[1] : &d->ht[0];
    size_t idx   = d->type->hash(key) & t->size_mask;
    dictEntry *e = malloc(sizeof(*e));
    if (!e) return -1;
    e->key = d->type->key_dup ? d->type->key_dup(key) : key;
    e->v.val = d->type->val_dup ? d->type->val_dup(val) : val;
    e->next  = t->buckets[idx];
    t->buckets[idx] = e;
    t->used++;
    return 0;
}

int dict_replace(dict *d, void *key, void *val) {
    dictEntry *e = dict_find(d, key);
    if (e) {
        if (d->type->val_free) d->type->val_free(e->v.val);
        e->v.val = d->type->val_dup ? d->type->val_dup(val) : val;
        return 0; /* updated */
    }
    return dict_add(d, key, val); /* inserted */
}

int dict_delete(dict *d, const void *key) {
    for (int t = 0; t <= 1; t++) {
        dictTable *tb = &d->ht[t];
        if (!tb->size) continue;
        size_t idx = d->type->hash(key) & tb->size_mask;
        dictEntry *e    = tb->buckets[idx];
        dictEntry *prev = NULL;
        while (e) {
            if (d->type->key_cmp(e->key, key) == 0) {
                if (prev) prev->next = e->next;
                else       tb->buckets[idx] = e->next;
                if (d->type->key_free) d->type->key_free(e->key);
                if (d->type->val_free) d->type->val_free(e->v.val);
                free(e);
                tb->used--;
                return 0;
            }
            prev = e;
            e    = e->next;
        }
        if (d->rehash_idx == -1) break;
    }
    return -1; /* not found */
}

/* ------------------------------------------------------------------ */
/* Iterator                                                            */
/* ------------------------------------------------------------------ */

dictIterator *dict_iter_new(dict *d) {
    dictIterator *it = calloc(1, sizeof(*it));
    if (!it) return NULL;
    it->d     = d;
    it->table = 0;
    it->idx   = -1;
    d->iterators++;
    return it;
}

dictEntry *dict_iter_next(dictIterator *it) {
    while (1) {
        if (it->entry) {
            it->entry = it->next_entry;
        } else {
            dictTable *t = &it->d->ht[it->table];
            it->idx++;
            if ((size_t)it->idx >= t->size) {
                if (it->table == 0 && it->d->rehash_idx != -1) {
                    it->table = 1;
                    it->idx   = 0;
                    t = &it->d->ht[1];
                } else {
                    return NULL;
                }
            }
            it->entry = t->buckets[it->idx];
        }
        if (it->entry) {
            it->next_entry = it->entry->next;
            return it->entry;
        }
    }
}

void dict_iter_free(dictIterator *it) {
    it->d->iterators--;
    free(it);
}
