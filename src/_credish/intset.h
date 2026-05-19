#ifndef CREDISH_INTSET_H
#define CREDISH_INTSET_H

/* Integer-optimised set encoding (placeholder — used when all members are integers) */
#include <stdint.h>
#include <stddef.h>

typedef struct intset {
    int64_t *contents;
    size_t   length;
} intset;

intset *intset_create(void);
void    intset_free(intset *s);
int     intset_add(intset *s, int64_t value);
int     intset_remove(intset *s, int64_t value);
int     intset_find(const intset *s, int64_t value);

#endif /* CREDISH_INTSET_H */
