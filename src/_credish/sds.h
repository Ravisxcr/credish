#ifndef CREDISH_SDS_H
#define CREDISH_SDS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Simple Dynamic String — binary-safe, length-prefixed string.
 * The sds typedef is a char* pointing just past the header, so it can be
 * passed directly to str functions while still tracking its own length.
 */

typedef char *sds;

/* Header stored before the character data. */
#ifdef _MSC_VER
#pragma pack(push, 1)
typedef struct sdshdr {
#else
typedef struct __attribute__((packed)) sdshdr {
#endif
    uint32_t len;   /* used bytes           */
    uint32_t alloc; /* allocated bytes (excl. header + null terminator) */
    char     buf[]; /* actual string data (null-terminated)             */
} sdshdr;
#ifdef _MSC_VER
#pragma pack(pop)
#endif

#define SDS_HDR(s)  ((sdshdr *)((s) - sizeof(sdshdr)))
#define SDS_LEN(s)  (SDS_HDR(s)->len)
#define SDS_AVAIL(s) (SDS_HDR(s)->alloc - SDS_HDR(s)->len)

sds   sds_new(const char *init, size_t initlen);
sds   sds_newlen(const void *init, size_t initlen);
sds   sds_empty(void);
sds   sds_dup(const sds s);
void  sds_free(sds s);
sds   sds_cat(sds s, const char *t, size_t len);
sds   sds_catprintf(sds s, const char *fmt, ...);
int   sds_cmp(const sds a, const sds b);
void  sds_clear(sds s);
sds   sds_grow(sds s, size_t addlen);

#endif /* CREDISH_SDS_H */
