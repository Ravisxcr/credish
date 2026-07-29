#include "intset.h"
#include <stdlib.h>
#include <string.h>

intset *intset_create(void) {
    return calloc(1, sizeof(intset));
}

void intset_free(intset *set) {
    if (set) { free(set->contents); free(set); }
}

static int intset_search(const intset *set, int64_t value, size_t *pos) {
    if (set->length == 0) { if (pos) *pos = 0; return 0; }
    size_t lo = 0, hi = set->length - 1;
    while (lo <= hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (set->contents[mid] == value) { if (pos) *pos = mid; return 1; }
        else if (set->contents[mid] < value) lo = mid + 1;
        else { if (mid == 0) break; hi = mid - 1; }
    }
    if (pos) *pos = lo;
    return 0;
}

int intset_add(intset *set, int64_t value) {
    size_t pos;
    if (intset_search(set, value, &pos)) return 0;
    set->contents = realloc(set->contents, (set->length + 1) * sizeof(int64_t));
    if (!set->contents) return -1;
    memmove(&set->contents[pos + 1], &set->contents[pos],
            (set->length - pos) * sizeof(int64_t));
    set->contents[pos] = value;
    set->length++;
    return 1;
}

int intset_remove(intset *set, int64_t value) {
    size_t pos;
    if (!intset_search(set, value, &pos)) return 0;
    memmove(&set->contents[pos], &set->contents[pos + 1],
            (set->length - pos - 1) * sizeof(int64_t));
    set->length--;
    return 1;
}

int intset_find(const intset *set, int64_t value) {
    return intset_search(set, value, NULL);
}
