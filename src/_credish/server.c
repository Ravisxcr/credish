#include "server.h"
#include <string.h>

persist_mode_t parse_persist_mode(const char *s) {
    if (!s) return PERSIST_HYBRID;
    if (strcmp(s, "none") == 0)   return PERSIST_NONE;
    if (strcmp(s, "rdb")  == 0)   return PERSIST_RDB;
    if (strcmp(s, "aof")  == 0)   return PERSIST_AOF;
    return PERSIST_HYBRID;
}

aof_fsync_t parse_aof_fsync(const char *s) {
    if (!s) return AOF_FSYNC_EVERYSEC;
    if (strcmp(s, "always")  == 0) return AOF_FSYNC_ALWAYS;
    if (strcmp(s, "no")      == 0) return AOF_FSYNC_NO;
    return AOF_FSYNC_EVERYSEC;
}
