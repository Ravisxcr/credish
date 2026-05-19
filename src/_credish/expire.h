#ifndef CREDISH_EXPIRE_H
#define CREDISH_EXPIRE_H

struct credish_store;

void expire_sweep_start(struct credish_store *s);
void expire_sweep_stop(struct credish_store *s);

#endif /* CREDISH_EXPIRE_H */
