#include "server.h"
#include <string.h>

persist_mode_t parse_persist_mode(const char *name) {
    if (!name) return PERSIST_HYBRID;
    if (strcmp(name, "none") == 0)   return PERSIST_NONE;
    if (strcmp(name, "rdb")  == 0)   return PERSIST_RDB;
    if (strcmp(name, "aof")  == 0)   return PERSIST_AOF;
    return PERSIST_HYBRID;
}

aof_fsync_t parse_aof_fsync(const char *name) {
    if (!name) return AOF_FSYNC_EVERYSEC;
    if (strcmp(name, "always")  == 0) return AOF_FSYNC_ALWAYS;
    if (strcmp(name, "no")      == 0) return AOF_FSYNC_NO;
    return AOF_FSYNC_EVERYSEC;
}
