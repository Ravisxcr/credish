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

#define SDS_HDR(str)  ((sdshdr *)((str) - sizeof(sdshdr)))
#define SDS_LEN(str)  (SDS_HDR(str)->len)
#define SDS_AVAIL(str) (SDS_HDR(str)->alloc - SDS_HDR(str)->len)

sds   sds_new(const char *init, size_t initlen);
sds   sds_newlen(const void *init, size_t initlen);
sds   sds_empty(void);
sds   sds_dup(const sds str);
void  sds_free(sds str);
sds   sds_cat(sds dest, const char *data, size_t len);
sds   sds_catprintf(sds dest, const char *fmt, ...);
int   sds_cmp(const sds a, const sds b);
void  sds_clear(sds str);
sds   sds_grow(sds str, size_t addlen);

#endif /* CREDISH_SDS_H */
