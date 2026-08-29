#include "dict.h"
#include "bufpool.h"
#include "sds.h"
#include <stdlib.h>
#include <string.h>

#define DICT_INITIAL_SIZE 4
#define DICT_LOAD_FACTOR 1

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int dict_expand(dict *dictionary, size_t size);
static void dict_rehash_step(dict *dictionary);
static dict_entry *dict_find_raw(dict *dictionary, const void *key, int table);

static size_t next_power(size_t n)
{
    size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

/* MurmurHash-inspired 64-bit mix */
uint64_t dict_hash_sds(const void *key)
{
    const sds sds_str = (const sds)key;
    size_t key_sds_len = SDS_LEN(sds_str);
    const uint8_t *data = (const uint8_t *)sds_str;
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < key_sds_len; i++)
    {
        hash ^= data[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

int dict_cmp_sds(const void *a, const void *b)
{
    return sds_compare((sds)a, (sds)b);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

dict *dict_create(dict_type *type)
{
    dict *dictionary = bufpool_alloc(sizeof(*dictionary));
    if (!dictionary)
        return NULL;

    memset(dictionary, 0, sizeof(*dictionary));
    dictionary->type = type;
    dictionary->rehash_idx = -1;
    return dictionary;
}

static void dict_table_free(dict *dictionary, int table)
{
    dict_table *bucket_table = &dictionary->ht[table];
    if (!bucket_table->buckets)
        return;
    for (size_t i = 0; i < bucket_table->size; i++)
    {
        dict_entry *entry = bucket_table->buckets[i];
        while (entry)
        {
            dict_entry *next = entry->next;
            if (dictionary->type->key_free)
                dictionary->type->key_free(entry->key);
            if (dictionary->type->val_free)
                dictionary->type->val_free(entry->v.val);
            bufpool_free(entry, sizeof(*entry));
            entry = next;
        }
    }
    free(bucket_table->buckets);
    memset(bucket_table, 0, sizeof(*bucket_table));
}

void dict_free(dict *dictionary)
{
    dict_table_free(dictionary, 0);
    dict_table_free(dictionary, 1);
    bufpool_free(dictionary, sizeof(*dictionary));
}

size_t dict_size(const dict *dictionary)
{
    return dictionary->ht[0].used + dictionary->ht[1].used;
}

/* ------------------------------------------------------------------ */
/* Rehashing                                                           */
/* ------------------------------------------------------------------ */

static int dict_expand(dict *dictionary, size_t size)
{
    if (dictionary->rehash_idx != -1)
        return -1; /* already rehashing */
    size_t real = next_power(size < DICT_INITIAL_SIZE ? DICT_INITIAL_SIZE : size);
    int is_rehash = (dictionary->ht[0].buckets != NULL);
    dict_table *bucket_table = &dictionary->ht[is_rehash ? 1 : 0];
    bucket_table->size = real;
    bucket_table->size_mask = real - 1;
    bucket_table->used = 0;
    bucket_table->buckets = calloc(real, sizeof(dict_entry *));
    if (!bucket_table->buckets)
        return -1;
    if (is_rehash)
        dictionary->rehash_idx = 0; /* start incremental rehash */
    return 0;
}

static void dict_rehash_step(dict *dictionary)
{
    if (dictionary->rehash_idx == -1 || dictionary->iterators)
        return;
    dict_table *src_dict_table = &dictionary->ht[0];
    dict_table *dst_dict_table = &dictionary->ht[1];

    /* Move one non-empty bucket */
    while ((size_t)dictionary->rehash_idx < src_dict_table->size && !src_dict_table->buckets[dictionary->rehash_idx])
        dictionary->rehash_idx++;
    if ((size_t)dictionary->rehash_idx >= src_dict_table->size)
        goto done;

    dict_entry *entry = src_dict_table->buckets[dictionary->rehash_idx];
    src_dict_table->buckets[dictionary->rehash_idx] = NULL;
    while (entry)
    {
        dict_entry *next = entry->next;
        size_t idx = dictionary->type->hash(entry->key) & dst_dict_table->size_mask;
        entry->next = dst_dict_table->buckets[idx];
        dst_dict_table->buckets[idx] = entry;
        src_dict_table->used--;
        dst_dict_table->used++;
        entry = next;
    }
    dictionary->rehash_idx++;

done:
    if (src_dict_table->used == 0)
    {
        free(src_dict_table->buckets);
        *src_dict_table = *dst_dict_table;
        memset(dst_dict_table, 0, sizeof(*dst_dict_table));
        dictionary->rehash_idx = -1;
    }
}

static void dict_maybe_expand(dict *dictionary)
{
    if (dictionary->rehash_idx != -1)
        return;
    if (!dictionary->ht[0].buckets)
    {
        dict_expand(dictionary, DICT_INITIAL_SIZE);
        return;
    }
    if (dictionary->ht[0].used >= dictionary->ht[0].size * DICT_LOAD_FACTOR)
        dict_expand(dictionary, dictionary->ht[0].used * 2);
}

/* ------------------------------------------------------------------ */
/* Core operations                                                     */
/* ------------------------------------------------------------------ */

static dict_entry *dict_find_raw(dict *dictionary, const void *key, int table)
{
    dict_table *bucket_table = &dictionary->ht[table];
    if (!bucket_table->size)
        return NULL;
    size_t idx = dictionary->type->hash(key) & bucket_table->size_mask;
    dict_entry *entry = bucket_table->buckets[idx];
    while (entry)
    {
        if (dictionary->type->key_cmp(entry->key, key) == 0)
            return entry;
        entry = entry->next;
    }
    return NULL;
}

dict_entry *dict_find(dict *dictionary, const void *key)
{
    if (dictionary->rehash_idx != -1)
        dict_rehash_step(dictionary);
    dict_entry *entry = dict_find_raw(dictionary, key, 0);
    if (!entry && dictionary->rehash_idx != -1)
        entry = dict_find_raw(dictionary, key, 1);
    return entry;
}

void *dict_fetch_value(dict *dictionary, const void *key)
{
    dict_entry *entry = dict_find(dictionary, key);
    return entry ? entry->v.val : NULL;
}

int dict_add(dict *dictionary, void *key, void *val)
{
    dict_maybe_expand(dictionary);
    if (dictionary->rehash_idx != -1)
        dict_rehash_step(dictionary);

    if (dict_find(dictionary, key))
        return -1; /* key already exists */

    dict_table *bucket_table = (dictionary->rehash_idx != -1) ? &dictionary->ht[1] : &dictionary->ht[0];
    size_t idx = dictionary->type->hash(key) & bucket_table->size_mask;
    dict_entry *entry = bufpool_alloc(sizeof(*entry));
    if (!entry)
        return -1;
    entry->key = dictionary->type->key_dup ? dictionary->type->key_dup(key) : key;
    entry->v.val = dictionary->type->val_dup ? dictionary->type->val_dup(val) : val;
    entry->next = bucket_table->buckets[idx];
    bucket_table->buckets[idx] = entry;
    bucket_table->used++;
    return 0;
}

int dict_replace(dict *dictionary, void *key, void *val)
{
    dict_entry *entry = dict_find(dictionary, key);
    if (entry)
    {
        if (dictionary->type->val_free)
            dictionary->type->val_free(entry->v.val);
        entry->v.val = dictionary->type->val_dup ? dictionary->type->val_dup(val) : val;
        return 0; /* updated */
    }
    return dict_add(dictionary, key, val); /* inserted */
}

int dict_delete(dict *dictionary, const void *key)
{
    for (int table = 0; table <= 1; table++)
    {
        dict_table *bucket_table = &dictionary->ht[table];
        if (!bucket_table->size)
            continue;
        size_t idx = dictionary->type->hash(key) & bucket_table->size_mask;
        dict_entry *curr_entry = bucket_table->buckets[idx];
        dict_entry *prev_entry = NULL;
        while (curr_entry)
        {
            if (dictionary->type->key_cmp(curr_entry->key, key) == 0)
            {
                if (prev_entry)
                    prev_entry->next = curr_entry->next;
                else
                    bucket_table->buckets[idx] = curr_entry->next;
                if (dictionary->type->key_free)
                    dictionary->type->key_free(curr_entry->key);
                if (dictionary->type->val_free)
                    dictionary->type->val_free(curr_entry->v.val);
                bufpool_free(curr_entry, sizeof(*curr_entry));
                bucket_table->used--;
                return 0;
            }
            prev_entry = curr_entry;
            curr_entry = curr_entry->next;
        }
        if (dictionary->rehash_idx == -1)
            break;
    }
    return -1; /* not found */
}

/* ------------------------------------------------------------------ */
/* Iterator                                                            */
/* ------------------------------------------------------------------ */

dict_iterator *dict_iter_new(dict *dictionary)
{
    dict_iterator *iter = bufpool_alloc(sizeof(*iter));
    if (!iter)
        return NULL;
    iter->dictionary = dictionary;
    iter->table = 0;
    iter->idx = -1;
    iter->entry = NULL;
    iter->next_entry = NULL;
    dictionary->iterators++;
    return iter;
}

dict_entry *dict_iter_next(dict_iterator *iter)
{
    while (1)
    {
        if (iter->entry)
        {
            iter->entry = iter->next_entry;
        }
        else
        {
            dict_table *bucket_table = &iter->dictionary->ht[iter->table];
            iter->idx++;
            if ((size_t)iter->idx >= bucket_table->size)
            {
                if (iter->table == 0 && iter->dictionary->rehash_idx != -1)
                {
                    iter->table = 1;
                    iter->idx = 0;
                    bucket_table = &iter->dictionary->ht[1];
                }
                else
                {
                    return NULL;
                }
            }
            iter->entry = bucket_table->buckets[iter->idx];
        }
        if (iter->entry)
        {
            iter->next_entry = iter->entry->next;
            return iter->entry;
        }
    }
}

void dict_iter_free(dict_iterator *it)
{
    it->dictionary->iterators--;
    bufpool_free(it, sizeof(*it));
}
