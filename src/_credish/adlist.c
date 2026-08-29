#include "adlist.h"
#include "bufpool.h"
#include <stdlib.h>
#include <string.h>

adlist *adlist_create(void)
{
    adlist *adlist_ptr = bufpool_alloc(sizeof(*adlist_ptr));
    if (adlist_ptr)
        memset(adlist_ptr, 0, sizeof(*adlist_ptr));
    return adlist_ptr;
}

void adlist_free(adlist *adlist_ptr, void (*free_val)(void *))
{
    adlist_node *curr_adlist_node_ptr = adlist_ptr->head;
    while (curr_adlist_node_ptr)
    {
        adlist_node *next_adlist_node_ptr = curr_adlist_node_ptr->next;
        if (free_val)
            free_val(curr_adlist_node_ptr->value);
        bufpool_free(curr_adlist_node_ptr, sizeof(*curr_adlist_node_ptr));
        curr_adlist_node_ptr = next_adlist_node_ptr;
    }
    bufpool_free(adlist_ptr, sizeof(*adlist_ptr));
}

static adlist_node *node_new(void *value)
{
    adlist_node *adlist_node_ptr = bufpool_alloc(sizeof(*adlist_node_ptr));
    if (!adlist_node_ptr)
        return NULL;
    adlist_node_ptr->value = value;
    adlist_node_ptr->prev = adlist_node_ptr->next = NULL;
    return adlist_node_ptr;
}

void adlist_push_head(adlist *adlist_ptr, void *value)
{
    adlist_node *adlist_node_ptr = node_new(value);
    if (!adlist_node_ptr)
        return;

    adlist_node_ptr->next = adlist_ptr->head;
    if (adlist_ptr->head)
        adlist_ptr->head->prev = adlist_node_ptr;
    adlist_ptr->head = adlist_node_ptr;

    if (!adlist_ptr->tail)
        adlist_ptr->tail = adlist_node_ptr;

    adlist_ptr->len++;
}

void adlist_push_tail(adlist *adlist_ptr, void *value)
{
    adlist_node *adlist_node_ptr = node_new(value);
    if (!adlist_node_ptr)
        return;

    adlist_node_ptr->prev = adlist_ptr->tail;
    if (adlist_ptr->tail)
        adlist_ptr->tail->next = adlist_node_ptr;

    adlist_ptr->tail = adlist_node_ptr;
    if (!adlist_ptr->head)
        adlist_ptr->head = adlist_node_ptr;

    adlist_ptr->len++;
}

void *adlist_pop_head(adlist *adlist_ptr)
{
    if (!adlist_ptr->head)
        return NULL;

    adlist_node *adlist_node_ptr = adlist_ptr->head;
    void *node_value = adlist_node_ptr->value;
    adlist_ptr->head = adlist_node_ptr->next;

    if (adlist_ptr->head)
        adlist_ptr->head->prev = NULL;
    else
        adlist_ptr->tail = NULL;

    bufpool_free(adlist_node_ptr, sizeof(*adlist_node_ptr));
    adlist_ptr->len--;

    return node_value;
}

void *adlist_pop_tail(adlist *adlist_ptr)
{
    if (!adlist_ptr->tail)
        return NULL;

    adlist_node *adlist_node_ptr = adlist_ptr->tail;
    void *node_value = adlist_node_ptr->value;
    adlist_ptr->tail = adlist_node_ptr->prev;

    if (adlist_ptr->tail)
        adlist_ptr->tail->next = NULL;
    else
        adlist_ptr->head = NULL;

    bufpool_free(adlist_node_ptr, sizeof(*adlist_node_ptr));
    adlist_ptr->len--;

    return node_value;
}

adlist_node *adlist_index(adlist *adlist_ptr, long index)
{
    adlist_node *curr_adlist_node_ptr;
    if (index >= 0)
    {
        curr_adlist_node_ptr = adlist_ptr->head;
        while (curr_adlist_node_ptr && index--)
            curr_adlist_node_ptr = curr_adlist_node_ptr->next;
    }
    else
    {
        curr_adlist_node_ptr = adlist_ptr->tail;
        index = (-index) - 1;
        while (curr_adlist_node_ptr && index--)
            curr_adlist_node_ptr = curr_adlist_node_ptr->prev;
    }
    return curr_adlist_node_ptr;
}

void adlist_delete_node(adlist *adlist_ptr, adlist_node *adlist_node_ptr, void (*free_val)(void *))
{
    if (adlist_node_ptr->prev)
        adlist_node_ptr->prev->next = adlist_node_ptr->next;
    else
        adlist_ptr->head = adlist_node_ptr->next;

    if (adlist_node_ptr->next)
        adlist_node_ptr->next->prev = adlist_node_ptr->prev;
    else
        adlist_ptr->tail = adlist_node_ptr->prev;

    if (free_val)
        free_val(adlist_node_ptr->value);
    bufpool_free(adlist_node_ptr, sizeof(*adlist_node_ptr));

    adlist_ptr->len--;
}

int adlist_rem(adlist *adlist_ptr, long limit, const void *value,
               int (*compare)(const void *, const void *),
               void (*free_val)(void *))
{
    int removed_count = 0;
    int forward_direction = limit >= 0;
    long max_to_remove = limit < 0 ? -limit : limit;
    adlist_node *curr_adlist_node = forward_direction ? adlist_ptr->head : adlist_ptr->tail;
    while (curr_adlist_node && (limit == 0 || removed_count < max_to_remove))
    {
        adlist_node *next = forward_direction ? curr_adlist_node->next : curr_adlist_node->prev;
        if (compare(curr_adlist_node->value, value) == 0)
        {
            adlist_delete_node(adlist_ptr, curr_adlist_node, free_val);
            removed_count++;
        }
        curr_adlist_node = next;
    }
    return removed_count;
}
