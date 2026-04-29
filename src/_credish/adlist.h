#ifndef CREDISH_ADLIST_H
#define CREDISH_ADLIST_H

#include <stddef.h>

typedef struct listNode {
    struct listNode *prev;
    struct listNode *next;
    void            *value;
} listNode;

typedef struct adlist {
    listNode *head;
    listNode *tail;
    size_t    len;
} adlist;

adlist   *adlist_create(void);
void      adlist_free(adlist *l, void (*free_val)(void *));
void      adlist_push_head(adlist *l, void *value);
void      adlist_push_tail(adlist *l, void *value);
void     *adlist_pop_head(adlist *l);
void     *adlist_pop_tail(adlist *l);
listNode *adlist_index(adlist *l, long index);
void      adlist_delete_node(adlist *l, listNode *node, void (*free_val)(void *));
int       adlist_rem(adlist *l, long count, const void *value,
                     int (*cmp)(const void *, const void *),
                     void (*free_val)(void *));

#endif /* CREDISH_ADLIST_H */
