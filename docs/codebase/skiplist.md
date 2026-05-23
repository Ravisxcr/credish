# Skip List: `skiplist.c`

## Overview

[`src/_credish/skiplist.c`](../../src/_credish/skiplist.c) implements the
ordered index for sorted-set values. A `zset` combines this index with a
dictionary mapping members to scores; see [`object.c`](object.md) and
[`sorted_set.c`](sorted_set.md).

## Ordering And Structure

Nodes are ordered first by ascending `double score`, then lexically by SDS
member for equal scores. Each node contains:

- A `member` SDS owned by the skip list.
- A score.
- A backward pointer for reverse traversal.
- A variable-size array of forward pointers and spans.

`ZSKIPLIST_MAXLEVEL` is 32 and new node levels are selected randomly with a
promotion probability of `0.25`.

## Operations

| Function | Behavior |
| --- | --- |
| `zsl_create()` / `zsl_free()` | Create a sentinel-headed list or release all nodes and members. |
| `zsl_insert()` | Insert a `(score, member)` node while updating spans and tail links. |
| `zsl_delete()` | Remove an exact `(score, member)` node. |
| `zsl_get_rank()` | Return a one-based rank, or zero when absent. |
| `zsl_get_element_by_rank()` | Resolve a one-based rank through spans. |

Spans let rank operations skip multiple nodes at higher levels instead of
walking the entire level-zero chain.

## Ownership

The caller supplies the member SDS to `zsl_insert()`, and the skip list takes
ownership of it. Sorted-set callers pass `sds_dup(member)` because their score
dictionary owns a separate copy of the same member.

## Allocation

Skip-list nodes are allocated with `malloc()` because their size depends on
their randomly selected level count; node teardown uses `free()`.

