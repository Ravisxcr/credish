#include "skiplist.h"
#include <stdlib.h>
#include <string.h>

static int zsl_random_level(void)
{
    int level = 1;
    while ((rand() & 0xFFFF) < (int)(ZSKIPLIST_P * 0xFFFF))
        level++;
    return level < ZSKIPLIST_MAXLEVEL ? level : ZSKIPLIST_MAXLEVEL;
}

static zskiplist_node *zsl_node_create(int level, double score, sds member)
{
    zskiplist_node *zsl_node = malloc(sizeof(*zsl_node) + level * sizeof(zsl_node->level[0]));
    if (!zsl_node)
        return NULL;
    zsl_node->score = score;
    zsl_node->member = member;
    zsl_node->backward = NULL;
    for (int i = 0; i < level; i++)
    {
        zsl_node->level[i].forward = NULL;
        zsl_node->level[i].span = 0;
    }
    return zsl_node;
}

zskiplist *zsl_create(void)
{
    zskiplist *zsl = malloc(sizeof(*zsl));
    if (!zsl)
        return NULL;
    zsl->level = 1;
    zsl->length = 0;
    /* Sentinel header: no member, score -inf */
    zsl->header = zsl_node_create(ZSKIPLIST_MAXLEVEL, 0, NULL);
    zsl->tail = NULL;
    return zsl;
}

void zsl_free(zskiplist *zsl_ptr)
{
    zskiplist_node *curr_zsl_node = zsl_ptr->header->level[0].forward;
    free(zsl_ptr->header);
    while (curr_zsl_node)
    {
        zskiplist_node *next_zsl_node = curr_zsl_node->level[0].forward;
        sds_free(curr_zsl_node->member);
        free(curr_zsl_node);
        curr_zsl_node = next_zsl_node;
    }
    free(zsl_ptr);
}

/* Returns 1 if (score1,member1) > (score2,member2) */
static int node_gt(double score1, sds member1, double score2, sds member2)
{
    if (score1 != score2)
        return score1 > score2;
    return sds_compare(member1, member2) > 0;
}

/* Returns 1 if (score1,member1) < (score2,member2) */
static int node_lt(double score1, sds member1, double score2, sds member2)
{
    if (score1 != score2)
        return score1 < score2;
    return sds_compare(member1, member2) < 0;
}

zskiplist_node *zsl_insert(zskiplist *zsl_ptr, double score, sds member)
{
    zskiplist_node *predecessors[ZSKIPLIST_MAXLEVEL];
    unsigned int prefix_ranks[ZSKIPLIST_MAXLEVEL];
    zskiplist_node *curr_zsl_header = zsl_ptr->header;

    for (int node_idx = zsl_ptr->level - 1; node_idx >= 0; node_idx--)
    {
        prefix_ranks[node_idx] = node_idx == zsl_ptr->level - 1 ? 0 : prefix_ranks[node_idx + 1];
        while (curr_zsl_header->level[node_idx].forward &&
               node_lt(curr_zsl_header->level[node_idx].forward->score,
                       curr_zsl_header->level[node_idx].forward->member,
                       score, member))
        {
            prefix_ranks[node_idx] += curr_zsl_header->level[node_idx].span;
            curr_zsl_header = curr_zsl_header->level[node_idx].forward;
        }
        predecessors[node_idx] = curr_zsl_header;
    }

    int level = zsl_random_level();
    if (level > zsl_ptr->level)
    {
        for (int i = zsl_ptr->level; i < level; i++)
        {
            prefix_ranks[i] = 0;
            predecessors[i] = zsl_ptr->header;
            predecessors[i]->level[i].span = zsl_ptr->length;
        }
        zsl_ptr->level = level;
    }

    curr_zsl_header = zsl_node_create(level, score, member);
    for (int i = 0; i < level; i++)
    {
        curr_zsl_header->level[i].forward = predecessors[i]->level[i].forward;
        predecessors[i]->level[i].forward = curr_zsl_header;
        curr_zsl_header->level[i].span = predecessors[i]->level[i].span - (prefix_ranks[0] - prefix_ranks[i]);
        predecessors[i]->level[i].span = (prefix_ranks[0] - prefix_ranks[i]) + 1;
    }
    for (int i = level; i < zsl_ptr->level; i++)
        predecessors[i]->level[i].span++;

    curr_zsl_header->backward = (predecessors[0] == zsl_ptr->header) ? NULL : predecessors[0];
    if (curr_zsl_header->level[0].forward)
        curr_zsl_header->level[0].forward->backward = curr_zsl_header;
    else
        zsl_ptr->tail = curr_zsl_header;
    zsl_ptr->length++;
    return curr_zsl_header;
}

int zsl_delete(zskiplist *zsl_ptr, double score, sds member)
{
    zskiplist_node *predecessors[ZSKIPLIST_MAXLEVEL];
    zskiplist_node *curr_zsl_header = zsl_ptr->header;
    for (int i = zsl_ptr->level - 1; i >= 0; i--)
    {
        while (curr_zsl_header->level[i].forward &&
               node_lt(curr_zsl_header->level[i].forward->score,
                       curr_zsl_header->level[i].forward->member, score, member))
            curr_zsl_header = curr_zsl_header->level[i].forward;
        predecessors[i] = curr_zsl_header;
    }
    curr_zsl_header = curr_zsl_header->level[0].forward;
    if (!curr_zsl_header || curr_zsl_header->score != score || sds_compare(curr_zsl_header->member, member) != 0)
        return 0;

    for (int i = 0; i < zsl_ptr->level; i++)
    {
        if (predecessors[i]->level[i].forward != curr_zsl_header)
        {
            predecessors[i]->level[i].span--;
            continue;
        }
        predecessors[i]->level[i].span += curr_zsl_header->level[i].span - 1;
        predecessors[i]->level[i].forward = curr_zsl_header->level[i].forward;
    }
    if (curr_zsl_header->level[0].forward)
        curr_zsl_header->level[0].forward->backward = curr_zsl_header->backward;
    else
        zsl_ptr->tail = curr_zsl_header->backward;

    while (zsl_ptr->level > 1 && !zsl_ptr->header->level[zsl_ptr->level - 1].forward)
        zsl_ptr->level--;
    sds_free(curr_zsl_header->member);
    free(curr_zsl_header);
    zsl_ptr->length--;
    return 1;
}

unsigned long zsl_get_rank(zskiplist *zsl_ptr, double score, sds member)
{
    zskiplist_node *curr_zsl_header = zsl_ptr->header;
    unsigned long accumulated_rank = 0;
    for (int i = zsl_ptr->level - 1; i >= 0; i--)
    {
        while (curr_zsl_header->level[i].forward &&
               !node_gt(curr_zsl_header->level[i].forward->score,
                        curr_zsl_header->level[i].forward->member,
                        score, member))
        {
            accumulated_rank += curr_zsl_header->level[i].span;
            curr_zsl_header = curr_zsl_header->level[i].forward;
        }
        if (curr_zsl_header->member && sds_compare(curr_zsl_header->member, member) == 0)
            return accumulated_rank;
    }
    return 0;
}

zskiplist_node *zsl_get_element_by_rank(zskiplist *zsl, unsigned long rank)
{
    zskiplist_node *curr_zsl_header = zsl->header;
    unsigned long accumulated_rank = 0;
    for (int i = zsl->level - 1; i >= 0; i--)
    {
        while (curr_zsl_header->level[i].forward && accumulated_rank + curr_zsl_header->level[i].span <= rank)
        {
            accumulated_rank += curr_zsl_header->level[i].span;
            curr_zsl_header = curr_zsl_header->level[i].forward;
        }
        if (accumulated_rank == rank)
            return curr_zsl_header;
    }
    return NULL;
}
