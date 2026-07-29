#include "sds.h"
#include "bufpool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static sds sds_alloc(size_t len, size_t alloc) {
    sdshdr *hdr = bufpool_alloc(sizeof(sdshdr) + alloc + 1);
    if (!hdr) return NULL;
    hdr->len = (uint32_t)len;
    hdr->alloc = (uint32_t)alloc;
    return hdr->buf;
}


sds sds_newlen(const void *init, size_t initlen) {
    sds str = sds_alloc(initlen, initlen);

    if (!str) return NULL;

    if (init && initlen)
        memcpy(str, init, initlen);

    str[initlen] = '\0';

    return str;
}

sds sds_new(const char *init, size_t initlen) {
    return sds_newlen(init, initlen);
}

sds sds_empty(void) {
    return sds_newlen("", 0);
}

sds sds_dup(const sds str) {
    return sds_newlen(str, SDS_LEN(str));
}

void sds_free(sds str) {
    if (!str) return;
    sdshdr *hdr = SDS_HDR(str);
    bufpool_free(hdr, sizeof(sdshdr) + hdr->alloc + 1);
}

void sds_clear(sds str) {
    SDS_HDR(str)->len = 0;
    str[0] = '\0';
}

sds sds_grow(sds str, size_t addlen) {
    sdshdr *hdr = SDS_HDR(str);
    size_t cur_alloc = hdr->alloc;
    size_t needed    = hdr->len + addlen;
    if (cur_alloc >= needed) return str;
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

sds sds_cat(sds dest, const char *data, size_t len) {
    size_t cur_len = SDS_LEN(dest);
    dest = sds_grow(dest, len);
    if (!dest) return NULL;
    memcpy(dest + cur_len, data, len);
    SDS_HDR(dest)->len = (uint32_t)(cur_len + len);
    dest[cur_len + len] = '\0';
    return dest;
}

sds sds_catprintf(sds dest, const char *fmt, ...) {
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return dest;
    return sds_cat(dest, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

int sds_cmp(const sds a, const sds b) {
    size_t la = SDS_LEN(a), lb = SDS_LEN(b);
    int cmp = memcmp(a, b, la < lb ? la : lb);
    if (cmp) return cmp;
    if (la < lb) return -1;
    if (la > lb) return  1;
    return 0;
}
