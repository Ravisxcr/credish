#ifndef CREDISH_RDB_H
#define CREDISH_RDB_H

struct credish_store;

int rdb_save(struct credish_store *store);
int rdb_load(struct credish_store *store);
int rdb_bgsave(struct credish_store *store);

#endif /* CREDISH_RDB_H */
