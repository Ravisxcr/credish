#include "expire.h"
#include "db.h"
#include "sds.h"
#include <pthread.h>
#include <time.h>
#include <stdlib.h>

#define SWEEP_INTERVAL_MS  100   /* sweep every 100 ms                 */
#define SWEEP_SAMPLE_SIZE  20    /* keys to sample per db per cycle    */
#define SWEEP_STOP_RATIO   0.25  /* stop if < 25% of samples expired   */

static void sweep_db(credish_db *db) {
    if (dict_size(db->expires) == 0) return;

    dictIterator *it = dict_iter_new(db->expires);
    dictEntry    *e;

    /* Collect up to SWEEP_SAMPLE_SIZE expired keys */
    sds to_del[SWEEP_SAMPLE_SIZE];
    int found = 0;

    while ((e = dict_iter_next(it)) != NULL && found < SWEEP_SAMPLE_SIZE) {
        int64_t dl = *(int64_t *)e->v.val;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        int64_t now = (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
        if (now > dl)
            to_del[found++] = sds_dup((sds)e->key);
    }
    dict_iter_free(it);

    for (int i = 0; i < found; i++) {
        dict_delete(db->keys, to_del[i]);
        dict_delete(db->expires, to_del[i]);
        sds_free(to_del[i]);
    }
}

static void *sweep_thread_fn(void *arg) {
    credish_store *s = (credish_store *)arg;
    struct timespec interval = {
        .tv_sec  = 0,
        .tv_nsec = SWEEP_INTERVAL_MS * 1000000L,
    };
    while (s->sweep_running) {
        nanosleep(&interval, NULL);
        pthread_rwlock_wrlock(&s->lock);
        for (int i = 0; i < CREDISH_DB_COUNT; i++)
            sweep_db(&s->dbs[i]);
        pthread_rwlock_unlock(&s->lock);
    }
    return NULL;
}

void expire_sweep_start(credish_store *s) {
    s->sweep_running = 1;
    pthread_create(&s->sweep_thread, NULL, sweep_thread_fn, s);
}

void expire_sweep_stop(credish_store *s) {
    s->sweep_running = 0;
    pthread_join(s->sweep_thread, NULL);
}
