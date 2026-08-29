#ifndef CREDISH_ADLIST_H
#define CREDISH_ADLIST_H

#include <stddef.h>

typedef struct adlist_node
{
    struct adlist_node *prev;
    struct adlist_node *next;
    void *value;
} adlist_node;

typedef struct adlist
{
    adlist_node *head;
    adlist_node *tail;
    size_t len;
} adlist;

adlist *adlist_create(void);
void adlist_free(adlist *list, void (*free_val)(void *));
void adlist_push_head(adlist *list, void *value);
void adlist_push_tail(adlist *list, void *value);
void *adlist_pop_head(adlist *list);
void *adlist_pop_tail(adlist *list);
adlist_node *adlist_index(adlist *list, long index);
void adlist_delete_node(adlist *list, adlist_node *node, void (*free_val)(void *));
int adlist_rem(adlist *list, long count, const void *value,
               int (*cmp)(const void *, const void *),
               void (*free_val)(void *));

#endif /* CREDISH_ADLIST_H */
