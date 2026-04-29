#ifndef CREDISH_AOF_H
#define CREDISH_AOF_H

struct credish_store;

int aof_open(struct credish_store *s);
int aof_load(struct credish_store *s);
void aof_fsync_bg(struct credish_store *s);  /* call from everysec timer */

#endif /* CREDISH_AOF_H */
