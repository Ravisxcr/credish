#ifndef CREDISH_EXPIRE_H
#define CREDISH_EXPIRE_H

struct credish_store;

void expire_sweep_start(struct credish_store *store);
void expire_sweep_stop(struct credish_store *store);

#endif /* CREDISH_EXPIRE_H */
