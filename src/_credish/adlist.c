#include "adlist.h"
#include "bufpool.h"
#include <stdlib.h>
#include <string.h>

adlist *adlist_create(void) {
    adlist *list = bufpool_alloc(sizeof(*list));
    if (list) memset(list, 0, sizeof(*list));
    return list;
}

void adlist_free(adlist *list, void (*free_val)(void *)) {
    listNode *node = list->head;
    while (node) {
        listNode *next = node->next;
        if (free_val) free_val(node->value);
        bufpool_free(node, sizeof(*node));
        node = next;
    }
    bufpool_free(list, sizeof(*list));
}

static listNode *node_new(void *value) {
    listNode *node = bufpool_alloc(sizeof(*node));
    if (!node) return NULL;
    node->value = value;
    node->prev  = node->next = NULL;
    return node;
}

void adlist_push_head(adlist *list, void *value) {
    listNode *node = node_new(value);
    if (!node) return;
    node->next = list->head;
    if (list->head) list->head->prev = node;
    list->head = node;
    if (!list->tail) list->tail = node;
    list->len++;
}

void adlist_push_tail(adlist *list, void *value) {
    listNode *node = node_new(value);
    if (!node) return;
    node->prev = list->tail;
    if (list->tail) list->tail->next = node;
    list->tail = node;
    if (!list->head) list->head = node;
    list->len++;
}

void *adlist_pop_head(adlist *list) {
    if (!list->head) return NULL;
    listNode *node = list->head;
    void *val = node->value;
    list->head = node->next;
    if (list->head) list->head->prev = NULL;
    else             list->tail = NULL;
    bufpool_free(node, sizeof(*node));
    list->len--;
    return val;
}

void *adlist_pop_tail(adlist *list) {
    if (!list->tail) return NULL;
    listNode *node = list->tail;
    void *val = node->value;
    list->tail = node->prev;
    if (list->tail) list->tail->next = NULL;
    else             list->head = NULL;
    bufpool_free(node, sizeof(*node));
    list->len--;
    return val;
}

listNode *adlist_index(adlist *list, long index) {
    listNode *node;
    if (index >= 0) {
        node = list->head;
        while (node && index--) node = node->next;
    } else {
        node = list->tail;
        index = (-index) - 1;
        while (node && index--) node = node->prev;
    }
    return node;
}

void adlist_delete_node(adlist *list, listNode *node, void (*free_val)(void *)) {
    if (node->prev) node->prev->next = node->next;
    else            list->head        = node->next;
    if (node->next) node->next->prev = node->prev;
    else            list->tail        = node->prev;
    if (free_val) free_val(node->value);
    bufpool_free(node, sizeof(*node));
    list->len--;
}

int adlist_rem(adlist *list, long count, const void *value,
               int (*cmp)(const void *, const void *),
               void (*free_val)(void *)) {
    int removed  = 0;
    int from_head = count >= 0;
    long abs_count = count < 0 ? -count : count;
    listNode *node = from_head ? list->head : list->tail;
    while (node && (count == 0 || removed < abs_count)) {
        listNode *next = from_head ? node->next : node->prev;
        if (cmp(node->value, value) == 0) {
            adlist_delete_node(list, node, free_val);
            removed++;
        }
        node = next;
    }
    return removed;
}
