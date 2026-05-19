#include "skiplist.h"
#include <stdlib.h>
#include <string.h>

static int zsl_random_level(void) {
    int level = 1;
    while ((rand() & 0xFFFF) < (int)(ZSKIPLIST_P * 0xFFFF))
        level++;
    return level < ZSKIPLIST_MAXLEVEL ? level : ZSKIPLIST_MAXLEVEL;
}

static zskiplistNode *zsl_node_create(int level, double score, sds member) {
    zskiplistNode *n = malloc(sizeof(*n) + level * sizeof(n->level[0]));
    if (!n) return NULL;
    n->score    = score;
    n->member   = member;
    n->backward = NULL;
    for (int i = 0; i < level; i++) {
        n->level[i].forward = NULL;
        n->level[i].span    = 0;
    }
    return n;
}

zskiplist *zsl_create(void) {
    zskiplist *zsl = malloc(sizeof(*zsl));
    if (!zsl) return NULL;
    zsl->level  = 1;
    zsl->length = 0;
    /* Sentinel header: no member, score -inf */
    zsl->header = zsl_node_create(ZSKIPLIST_MAXLEVEL, 0, NULL);
    zsl->tail   = NULL;
    return zsl;
}

void zsl_free(zskiplist *zsl) {
    zskiplistNode *n = zsl->header->level[0].forward;
    free(zsl->header);
    while (n) {
        zskiplistNode *next = n->level[0].forward;
        sds_free(n->member);
        free(n);
        n = next;
    }
    free(zsl);
}

/* Returns 1 if (score1,member1) > (score2,member2) */
static int node_gt(double s1, sds m1, double s2, sds m2) {
    if (s1 != s2) return s1 > s2;
    return sds_cmp(m1, m2) > 0;
}

/* Returns 1 if (score1,member1) < (score2,member2) */
static int node_lt(double s1, sds m1, double s2, sds m2) {
    if (s1 != s2) return s1 < s2;
    return sds_cmp(m1, m2) < 0;
}

zskiplistNode *zsl_insert(zskiplist *zsl, double score, sds member) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL];
    unsigned int   rank[ZSKIPLIST_MAXLEVEL];
    zskiplistNode *x = zsl->header;

    for (int i = zsl->level - 1; i >= 0; i--) {
        rank[i] = i == zsl->level - 1 ? 0 : rank[i + 1];
        while (x->level[i].forward &&
               node_lt(x->level[i].forward->score,
                       x->level[i].forward->member,
                       score, member)) {
            rank[i] += x->level[i].span;
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    int level = zsl_random_level();
    if (level > zsl->level) {
        for (int i = zsl->level; i < level; i++) {
            rank[i]           = 0;
            update[i]         = zsl->header;
            update[i]->level[i].span = zsl->length;
        }
        zsl->level = level;
    }

    x = zsl_node_create(level, score, member);
    for (int i = 0; i < level; i++) {
        x->level[i].forward         = update[i]->level[i].forward;
        update[i]->level[i].forward = x;
        x->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]);
        update[i]->level[i].span = (rank[0] - rank[i]) + 1;
    }
    for (int i = level; i < zsl->level; i++)
        update[i]->level[i].span++;

    x->backward = (update[0] == zsl->header) ? NULL : update[0];
    if (x->level[0].forward) x->level[0].forward->backward = x;
    else                      zsl->tail = x;
    zsl->length++;
    return x;
}

int zsl_delete(zskiplist *zsl, double score, sds member) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL];
    zskiplistNode *x = zsl->header;
    for (int i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward &&
               node_lt(x->level[i].forward->score,
                       x->level[i].forward->member, score, member))
            x = x->level[i].forward;
        update[i] = x;
    }
    x = x->level[0].forward;
    if (!x || x->score != score || sds_cmp(x->member, member) != 0) return 0;

    for (int i = 0; i < zsl->level; i++) {
        if (update[i]->level[i].forward != x) {
            update[i]->level[i].span--;
            continue;
        }
        update[i]->level[i].span   += x->level[i].span - 1;
        update[i]->level[i].forward = x->level[i].forward;
    }
    if (x->level[0].forward) x->level[0].forward->backward = x->backward;
    else                      zsl->tail = x->backward;

    while (zsl->level > 1 && !zsl->header->level[zsl->level - 1].forward)
        zsl->level--;
    sds_free(x->member);
    free(x);
    zsl->length--;
    return 1;
}

unsigned long zsl_get_rank(zskiplist *zsl, double score, sds member) {
    zskiplistNode *x = zsl->header;
    unsigned long rank = 0;
    for (int i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward &&
               !node_gt(x->level[i].forward->score,
                        x->level[i].forward->member,
                        score, member)) {
            rank += x->level[i].span;
            x = x->level[i].forward;
        }
        if (x->member && sds_cmp(x->member, member) == 0) return rank;
    }
    return 0;
}

zskiplistNode *zsl_get_element_by_rank(zskiplist *zsl, unsigned long rank) {
    zskiplistNode *x = zsl->header;
    unsigned long traversed = 0;
    for (int i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward && traversed + x->level[i].span <= rank) {
            traversed += x->level[i].span;
            x = x->level[i].forward;
        }
        if (traversed == rank) return x;
    }
    return NULL;
}
