#include "intset.h"
#include <stdlib.h>
#include <string.h>

intset *intset_create(void) {
    return calloc(1, sizeof(intset));
}

void intset_free(intset *s) {
    if (s) { free(s->contents); free(s); }
}

static int intset_search(const intset *s, int64_t value, size_t *pos) {
    if (s->length == 0) { if (pos) *pos = 0; return 0; }
    size_t lo = 0, hi = s->length - 1;
    while (lo <= hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s->contents[mid] == value) { if (pos) *pos = mid; return 1; }
        else if (s->contents[mid] < value) lo = mid + 1;
        else { if (mid == 0) break; hi = mid - 1; }
    }
    if (pos) *pos = lo;
    return 0;
}

int intset_add(intset *s, int64_t value) {
    size_t pos;
    if (intset_search(s, value, &pos)) return 0;
    s->contents = realloc(s->contents, (s->length + 1) * sizeof(int64_t));
    if (!s->contents) return -1;
    memmove(&s->contents[pos + 1], &s->contents[pos],
            (s->length - pos) * sizeof(int64_t));
    s->contents[pos] = value;
    s->length++;
    return 1;
}

int intset_remove(intset *s, int64_t value) {
    size_t pos;
    if (!intset_search(s, value, &pos)) return 0;
    memmove(&s->contents[pos], &s->contents[pos + 1],
            (s->length - pos - 1) * sizeof(int64_t));
    s->length--;
    return 1;
}

int intset_find(const intset *s, int64_t value) {
    return intset_search(s, value, NULL);
}
