#include "bufpool.h"
#include <stdlib.h>
#include <string.h>
#include "platform.h"

#define POOL_PAGE_BYTES  4096
#define POOL_NUM_CLASSES 12

static const size_t SCLASS[POOL_NUM_CLASSES] = {
    8, 16, 24, 32, 48, 64, 96, 128, 256, 512, 1024, 2048
};

/* Overlaid into free slots — must fit within the smallest size class (8 B). */
typedef struct free_slot { struct free_slot *next; } free_slot;

/* One OS page; the slab's data starts immediately after this header. */
typedef struct page { struct page *next; } page;

typedef struct {
    size_t          sz;
    free_slot      *free;   /* recycled-slot list                 */
    char           *bump;   /* next virgin byte in current page   */
    char           *end;    /* one past last usable byte of page  */
    page           *pages;  /* all allocated pages (for destroy)  */
    credish_mutex_t lock;
} slab;

static slab             g_slabs[POOL_NUM_CLASSES];
static credish_once_t   g_once = CREDISH_ONCE_INIT;

static void pool_init_once(void) {
    for (int i = 0; i < POOL_NUM_CLASSES; i++) {
        slab *sl   = &g_slabs[i];
        sl->sz     = SCLASS[i];
        sl->free   = NULL;
        sl->bump   = NULL;
        sl->end    = NULL;
        sl->pages  = NULL;
        credish_mutex_init(&sl->lock);
    }
}

void bufpool_init(void) {
    credish_once(&g_once, pool_init_once);
}

/* Round `size` up to the matching size-class index, or -1 if too large. */
static int sclass_idx(size_t size) {
    for (int i = 0; i < POOL_NUM_CLASSES; i++)
        if (size <= SCLASS[i]) return i;
    return -1;
}

static int grow_slab(slab *sl) {
    page *pg = malloc(POOL_PAGE_BYTES);
    if (!pg) return 0;
    pg->next  = sl->pages;
    sl->pages = pg;
    sl->bump  = (char *)(pg + 1);          /* data starts after header */
    sl->end   = (char *)pg + POOL_PAGE_BYTES;
    return 1;
}

void *bufpool_alloc(size_t size) {
    credish_once(&g_once, pool_init_once);

    int idx = sclass_idx(size);
    if (idx < 0) return malloc(size);

    slab *sl = &g_slabs[idx];
    credish_mutex_lock(&sl->lock);

    void *p;
    if (sl->free) {
        p        = sl->free;
        sl->free = sl->free->next;
    } else {
        if (sl->bump + sl->sz > sl->end && !grow_slab(sl)) {
            credish_mutex_unlock(&sl->lock);
            return NULL;
        }
        p        = sl->bump;
        sl->bump += sl->sz;
    }

    credish_mutex_unlock(&sl->lock);
    return p;
}

void bufpool_free(void *ptr, size_t size) {
    if (!ptr) return;
    credish_once(&g_once, pool_init_once);

    int idx = sclass_idx(size);
    if (idx < 0) { free(ptr); return; }

    slab *sl  = &g_slabs[idx];
    credish_mutex_lock(&sl->lock);
    free_slot *s = ptr;
    s->next  = sl->free;
    sl->free = s;
    credish_mutex_unlock(&sl->lock);
}

void bufpool_destroy(void) {
    for (int i = 0; i < POOL_NUM_CLASSES; i++) {
        slab *sl = &g_slabs[i];
        credish_mutex_lock(&sl->lock);
        page *pg = sl->pages;
        while (pg) {
            page *nx = pg->next;
            free(pg);
            pg = nx;
        }
        sl->pages = NULL;
        sl->free  = NULL;
        sl->bump  = NULL;
        sl->end   = NULL;
        credish_mutex_unlock(&sl->lock);
        credish_mutex_destroy(&sl->lock);
    }
}
