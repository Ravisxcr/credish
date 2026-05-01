#include "sds.h"
#include "bufpool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static sds sds_alloc(size_t len, size_t alloc) {
    sdshdr *hdr = bufpool_alloc(sizeof(sdshdr) + alloc + 1);
    if (!hdr) return NULL;
    hdr->len   = (uint32_t)len;
    hdr->alloc = (uint32_t)alloc;
    return hdr->buf;
}

sds sds_newlen(const void *init, size_t initlen) {
    sds s = sds_alloc(initlen, initlen);
    if (!s) return NULL;
    if (init && initlen)
        memcpy(s, init, initlen);
    s[initlen] = '\0';
    return s;
}

sds sds_new(const char *init, size_t initlen) {
    return sds_newlen(init, initlen);
}

sds sds_empty(void) {
    return sds_newlen("", 0);
}

sds sds_dup(const sds s) {
    return sds_newlen(s, SDS_LEN(s));
}

void sds_free(sds s) {
    if (!s) return;
    sdshdr *hdr = SDS_HDR(s);
    bufpool_free(hdr, sizeof(sdshdr) + hdr->alloc + 1);
}

void sds_clear(sds s) {
    SDS_HDR(s)->len = 0;
    s[0] = '\0';
}

sds sds_grow(sds s, size_t addlen) {
    sdshdr *hdr = SDS_HDR(s);
    size_t cur_alloc = hdr->alloc;
    size_t needed    = hdr->len + addlen;
    if (cur_alloc >= needed) return s;
    size_t new_alloc = cur_alloc ? cur_alloc : 1;
    while (new_alloc < needed) new_alloc *= 2;
    /* Can't realloc a pool-owned slab: alloc new, copy, free old. */
    sdshdr *new_hdr = bufpool_alloc(sizeof(sdshdr) + new_alloc + 1);
    if (!new_hdr) return NULL;
    memcpy(new_hdr, hdr, sizeof(sdshdr) + hdr->len + 1);
    new_hdr->alloc = (uint32_t)new_alloc;
    bufpool_free(hdr, sizeof(sdshdr) + cur_alloc + 1);
    return new_hdr->buf;
}

sds sds_cat(sds s, const char *t, size_t len) {
    size_t cur_len = SDS_LEN(s);
    s = sds_grow(s, len);
    if (!s) return NULL;
    memcpy(s + cur_len, t, len);
    SDS_HDR(s)->len = (uint32_t)(cur_len + len);
    s[cur_len + len] = '\0';
    return s;
}

sds sds_catprintf(sds s, const char *fmt, ...) {
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return s;
    return sds_cat(s, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

int sds_cmp(const sds a, const sds b) {
    size_t la = SDS_LEN(a), lb = SDS_LEN(b);
    int cmp = memcmp(a, b, la < lb ? la : lb);
    if (cmp) return cmp;
    if (la < lb) return -1;
    if (la > lb) return  1;
    return 0;
}
