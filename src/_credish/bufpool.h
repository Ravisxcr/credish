#ifndef CREDISH_BUFPOOL_H
#define CREDISH_BUFPOOL_H

#include <stddef.h>

/*
 * Slab-based buffer pool.  Pre-allocates 4 KB pages per size class and
 * serves fixed-size allocations from a free list — O(1) alloc/free with
 * no system-call overhead on the hot path.
 *
 * Size classes: 8, 16, 24, 32, 48, 64, 96, 128, 256, 512, 1024, 2048.
 * Requests larger than 2048 bytes fall back to system malloc/free.
 *
 * The same `size` value MUST be passed to both bufpool_alloc and
 * bufpool_free for the same pointer — the pool uses it to locate the
 * correct size class.  This matches the usage pattern of every fixed-size
 * struct in the codebase (credishObject, dictEntry, listNode, …) and also
 * the SDS allocator which stores alloc-size in its own header.
 */

void  bufpool_init(void);
void  bufpool_destroy(void);
void *bufpool_alloc(size_t size);
void  bufpool_free(void *ptr, size_t size);

#endif /* CREDISH_BUFPOOL_H */
