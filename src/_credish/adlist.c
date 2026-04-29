#include "adlist.h"
#include <stdlib.h>

adlist *adlist_create(void) {
    adlist *l = calloc(1, sizeof(*l));
    return l;
}

void adlist_free(adlist *l, void (*free_val)(void *)) {
    listNode *n = l->head;
    while (n) {
        listNode *next = n->next;
        if (free_val) free_val(n->value);
        free(n);
        n = next;
    }
    free(l);
}

static listNode *node_new(void *value) {
    listNode *n = malloc(sizeof(*n));
    if (!n) return NULL;
    n->value = value;
    n->prev  = n->next = NULL;
    return n;
}

void adlist_push_head(adlist *l, void *value) {
    listNode *n = node_new(value);
    if (!n) return;
    n->next = l->head;
    if (l->head) l->head->prev = n;
    l->head = n;
    if (!l->tail) l->tail = n;
    l->len++;
}

void adlist_push_tail(adlist *l, void *value) {
    listNode *n = node_new(value);
    if (!n) return;
    n->prev = l->tail;
    if (l->tail) l->tail->next = n;
    l->tail = n;
    if (!l->head) l->head = n;
    l->len++;
}

void *adlist_pop_head(adlist *l) {
    if (!l->head) return NULL;
    listNode *n = l->head;
    void *val   = n->value;
    l->head     = n->next;
    if (l->head) l->head->prev = NULL;
    else         l->tail = NULL;
    free(n);
    l->len--;
    return val;
}

void *adlist_pop_tail(adlist *l) {
    if (!l->tail) return NULL;
    listNode *n = l->tail;
    void *val   = n->value;
    l->tail     = n->prev;
    if (l->tail) l->tail->next = NULL;
    else         l->head = NULL;
    free(n);
    l->len--;
    return val;
}

listNode *adlist_index(adlist *l, long index) {
    listNode *n;
    if (index >= 0) {
        n = l->head;
        while (n && index--) n = n->next;
    } else {
        n = l->tail;
        index = (-index) - 1;
        while (n && index--) n = n->prev;
    }
    return n;
}

void adlist_delete_node(adlist *l, listNode *node, void (*free_val)(void *)) {
    if (node->prev) node->prev->next = node->next;
    else            l->head          = node->next;
    if (node->next) node->next->prev = node->prev;
    else            l->tail          = node->prev;
    if (free_val) free_val(node->value);
    free(node);
    l->len--;
}

int adlist_rem(adlist *l, long count, const void *value,
               int (*cmp)(const void *, const void *),
               void (*free_val)(void *)) {
    int removed  = 0;
    int from_head = count >= 0;
    long abs_count = count < 0 ? -count : count;
    listNode *n = from_head ? l->head : l->tail;
    while (n && (count == 0 || removed < abs_count)) {
        listNode *next = from_head ? n->next : n->prev;
        if (cmp(n->value, value) == 0) {
            adlist_delete_node(l, n, free_val);
            removed++;
        }
        n = next;
    }
    return removed;
}
