#include "sds.h"
#include "bufpool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static sds sds_alloc(size_t str_len, size_t alloc)
{
    sdshdr *sds_header = bufpool_alloc(sizeof(sdshdr) + alloc + 1);
    if (!sds_header)
        return NULL;
    sds_header->len = (uint32_t)str_len;
    sds_header->alloc = (uint32_t)alloc;
    return sds_header->buf;
}

sds sds_newlen(const void *init, size_t initlen)
{
    sds str = sds_alloc(initlen, initlen);
    if (!str)
        return NULL;

    if (init && initlen)
        memcpy(str, init, initlen);

    str[initlen] = '\0';
    return str;
}

sds sds_new(const char *init, size_t initlen)
{
    return sds_newlen(init, initlen);
}

sds sds_empty(void)
{
    return sds_newlen("", 0);
}

sds sds_dup(const sds str)
{
    return sds_newlen(str, SDS_LEN(str));
}

void sds_free(sds str)
{
    if (!str)
        return;
    sdshdr *sds_header = SDS_HDR(str);
    bufpool_free(sds_header, sizeof(sdshdr) + sds_header->alloc + 1);
}

void sds_clear(sds str)
{
    SDS_HDR(str)->len = 0;
    str[0] = '\0';
}

sds sds_grow(sds str, size_t append_len)
{
    sdshdr *sds_header = SDS_HDR(str);
    size_t curr_alloc = sds_header->alloc;
    size_t required_alloc = sds_header->len + append_len;

    // 1. If available capacity is already enough, do nothing
    if (curr_alloc >= required_alloc)
        return str;

    // 2. Overflow guard and hard limit check
    size_t curr_len = SDS_LEN(str);
    if (required_alloc < curr_len || required_alloc > SDS_MAX_ALLOC)
    {
        return NULL;
    }

    // 3. Capped growth policy (exponential vs linear)
    size_t new_alloc;
    if (required_alloc < SDS_PREALLOC_THRESHOLD)
    {
        new_alloc = required_alloc * 2;
    }
    else
    {
        new_alloc = required_alloc + SDS_PREALLOC_THRESHOLD;
    }

    // size_t new_alloc = curr_alloc ? curr_alloc : 1;
    // while (new_alloc < required_len)
    //     new_alloc *= 2;

    if (new_alloc > SDS_MAX_ALLOC)
    {
        new_alloc = SDS_MAX_ALLOC;
    }

    /* Can't realloc a pool-owned slab: alloc new, copy, free old. */
    sdshdr *new_sds_header = bufpool_alloc(sizeof(sdshdr) + new_alloc + 1);
    if (!new_sds_header)
        return NULL;

    memcpy(new_sds_header, sds_header, sizeof(sdshdr) + sds_header->len + 1);
    new_sds_header->alloc = (uint32_t)new_alloc;
    bufpool_free(sds_header, sizeof(sdshdr) + curr_alloc + 1);

    return new_sds_header->buf;
}

sds sds_catlen(sds dst_sds, const char *src_bytes, size_t append_len)
{
    size_t curr_len = SDS_LEN(dst_sds);
    sds target_sds = sds_grow(dst_sds, append_len);
    if (!target_sds)
        return NULL;

    memcpy(target_sds + curr_len, src_bytes, append_len);

    size_t total_len = curr_len + append_len;
    SDS_HDR(dst_sds)->len = (uint32_t)(total_len);
    dst_sds[total_len] = '\0';

    return target_sds;
}

sds sds_catlenprintf(sds dst_sds, const char *format_string, ...)
{
    va_list extra_args;
    char temp_buffer[1024];
    va_start(extra_args, format_string);
    int formatted_len = vsnprintf(temp_buffer, sizeof(temp_buffer), format_string, extra_args);
    va_end(extra_args);
    if (formatted_len < 0)
        return dst_sds;
    return sds_catlen(dst_sds, temp_buffer, (size_t)formatted_len < sizeof(temp_buffer) ? (size_t)formatted_len : sizeof(temp_buffer) - 1);
}

int sds_compare(const sds a, const sds b)
{
    size_t len_a = SDS_LEN(a), len_b = SDS_LEN(b);
    int cmp = memcmp(a, b, len_a < len_b ? len_a : len_b);
    return cmp != 0 ? cmp : (len_a > len_b) - (len_a < len_b);
}
