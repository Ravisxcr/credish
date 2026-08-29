#include "bufpool.h"
#include <stdlib.h>
#include <string.h>
#include "platform.h"

#define POOL_PAGE_BYTES 4096
#define POOL_NUM_CLASSES 12

static const size_t SCLASS[POOL_NUM_CLASSES] = {
    8, 16, 24, 32, 48, 64, 96, 128, 256, 512, 1024, 2048};

/* Overlaid into free slots — must fit within the smallest size class (8 B). */
typedef struct free_slot
{
    struct free_slot *next;
} free_slot;

/* One OS page; the slab's data starts immediately after this header. */
typedef struct page
{
    struct page *next;
} page;

typedef struct
{
    size_t sz;
    free_slot *free; /* recycled-slot list                 */
    char *bump;      /* next virgin byte in current page   */
    char *end;       /* one past last usable byte of page  */
    page *pages;     /* all allocated pages (for destroy)  */
    credish_mutex_t lock;
} slab;

static slab global_slabs[POOL_NUM_CLASSES];
static credish_once_t global_once = CREDISH_ONCE_INIT;

static void pool_init_once(void)
{
    for (int i = 0; i < POOL_NUM_CLASSES; i++)
    {
        slab *bucket = &global_slabs[i];
        bucket->sz = SCLASS[i];
        bucket->free = NULL;
        bucket->bump = NULL;
        bucket->end = NULL;
        bucket->pages = NULL;
        credish_mutex_init(&bucket->lock);
    }
}

void bufpool_init(void)
{
    credish_once(&global_once, pool_init_once);
}

/* Round `size` up to the matching size-class index, or -1 if too large. */
static int find_size_class_index(size_t size)
{
    for (int idx = 0; idx < POOL_NUM_CLASSES; idx++)
        if (size <= SCLASS[idx])
            return idx;
    return -1;
}

static int allocate_new_page(slab *bucket)
{
    page *new_page = malloc(POOL_PAGE_BYTES);
    if (!new_page)
        return 0;
    new_page->next = bucket->pages;
    bucket->pages = new_page;
    bucket->bump = (char *)(new_page + 1); /* data starts after header */
    bucket->end = (char *)new_page + POOL_PAGE_BYTES;
    return 1;
}

void *bufpool_alloc(size_t requested_bytes)
{
    credish_once(&global_once, pool_init_once);

    int size_class_idx = find_size_class_index(requested_bytes);
    if (size_class_idx < 0)
        return malloc(requested_bytes);

    slab *bucket = &global_slabs[size_class_idx];
    credish_mutex_lock(&bucket->lock);

    void *allocated_block;
    if (bucket->free)
    {
        allocated_block = bucket->free;
        bucket->free = bucket->free->next;
    }
    else
    {
        if (bucket->bump + bucket->sz > bucket->end && !allocate_new_page(bucket))
        {
            credish_mutex_unlock(&bucket->lock);
            return NULL;
        }
        allocated_block = bucket->bump;
        bucket->bump += bucket->sz;
    }

    credish_mutex_unlock(&bucket->lock);
    return allocated_block;
}

void bufpool_free(void *block_ptr, size_t size)
{
    if (!block_ptr)
        return;
    credish_once(&global_once, pool_init_once);

    int size_class_idx = find_size_class_index(size);
    if (size_class_idx < 0)
    {
        free(block_ptr);
        return;
    }

    slab *bucket = &global_slabs[size_class_idx];
    credish_mutex_lock(&bucket->lock);

    free_slot *free_slot_ptr = block_ptr;
    free_slot_ptr->next = bucket->free;
    bucket->free = free_slot_ptr;

    credish_mutex_unlock(&bucket->lock);
}

void bufpool_destroy(void)
{
    for (int i = 0; i < POOL_NUM_CLASSES; i++)
    {
        slab *bucket = &global_slabs[i];
        credish_mutex_lock(&bucket->lock);
        page *current_page = bucket->pages;
        while (current_page)
        {
            page *next_page = current_page->next;
            free(current_page);
            current_page = next_page;
        }
        bucket->pages = NULL;
        bucket->free = NULL;
        bucket->bump = NULL;
        bucket->end = NULL;

        credish_mutex_unlock(&bucket->lock);
        credish_mutex_destroy(&bucket->lock);
    }
}
