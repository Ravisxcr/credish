#ifndef CREDISH_SERVER_H
#define CREDISH_SERVER_H

#include <stdint.h>

typedef enum
{
    PERSIST_NONE = 0,
    PERSIST_RDB = 1,
    PERSIST_AOF = 2,
    PERSIST_HYBRID = 3,
} persist_mode_t;

typedef enum
{
    AOF_FSYNC_ALWAYS = 0,
    AOF_FSYNC_EVERYSEC = 1,
    AOF_FSYNC_NO = 2,
} aof_fsync_t;

typedef struct credish_config
{
    char data_dir[512];
    persist_mode_t persist_mode;
    int save_interval; /* seconds between RDB saves */
    aof_fsync_t aof_fsync;
    int decode_responses;
} credish_config;

persist_mode_t parse_persist_mode(const char *name);
aof_fsync_t parse_aof_fsync(const char *name);

#endif /* CREDISH_SERVER_H */
