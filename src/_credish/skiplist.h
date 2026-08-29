#ifndef CREDISH_SKIPLIST_H
#define CREDISH_SKIPLIST_H

#include "sds.h"
#include <stdint.h>

#define ZSKIPLIST_MAXLEVEL 32
#define ZSKIPLIST_P 0.25

typedef struct zskiplist_node
{
    sds member;
    double score;
    struct zskiplist_node *backward;
    struct
    {
        struct zskiplist_node *forward;
        unsigned int span;
    } level[];
} zskiplist_node;

typedef struct zskiplist
{
    zskiplist_node *header;
    zskiplist_node *tail;
    unsigned long length;
    int level;
} zskiplist;

zskiplist *zsl_create(void);
void zsl_free(zskiplist *zsl);
zskiplist_node *zsl_insert(zskiplist *zsl, double score, sds member);
int zsl_delete(zskiplist *zsl, double score, sds member);
unsigned long zsl_get_rank(zskiplist *zsl, double score, sds member);
zskiplist_node *zsl_get_element_by_rank(zskiplist *zsl, unsigned long rank);

#endif /* CREDISH_SKIPLIST_H */
