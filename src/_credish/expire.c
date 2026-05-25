#include "expire.h"
#include "db.h"
#include "sds.h"
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>

#define SWEEP_INTERVAL_MS  100   /* sweep every 100 ms                 */
#define SWEEP_SAMPLE_SIZE  20    /* keys to sample per db per cycle    */
#define SWEEP_STOP_NUM     1     /* stop if < 25% of samples expired   */
#define SWEEP_STOP_DEN     4
#define SWEEP_MAX_LOOPS    16    /* bound work per db per cycle        */

static int64_t sweep_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int sweep_db_sample(credish_db *db, unsigned int *seed) {
    size_t total = dict_size(db->expires);
    if (total == 0) return 0;

    size_t offset = (size_t)(rand_r(seed) % (unsigned int)total);
    dictIterator *it = dict_iter_new(db->expires);
    dictEntry    *e;

    sds to_del[SWEEP_SAMPLE_SIZE];
    int sampled = 0;
    int expired = 0;

    while (offset-- > 0 && dict_iter_next(it) != NULL) {
        /* Skip to a different point each cycle so cold expired keys do not
           depend on dictionary iteration order for cleanup. */
    }

    while ((e = dict_iter_next(it)) != NULL && sampled < SWEEP_SAMPLE_SIZE) {
        sampled++;
        int64_t dl = *(int64_t *)e->v.val;
        if (sweep_now_ms() > dl)
            to_del[expired++] = sds_dup((sds)e->key);
    }
    dict_iter_free(it);

    for (int i = 0; i < expired; i++) {
        dict_delete(db->keys, to_del[i]);
        dict_delete(db->expires, to_del[i]);
        sds_free(to_del[i]);
    }

    return sampled > 0 &&
           expired * SWEEP_STOP_DEN >= sampled * SWEEP_STOP_NUM;
}

static void sweep_db(credish_db *db, unsigned int *seed) {
    int loops = 0;
    while (loops++ < SWEEP_MAX_LOOPS && sweep_db_sample(db, seed)) {
        if (dict_size(db->expires) == 0) return;
    }
}

static void *sweep_thread_fn(void *arg) {
    credish_store *s = (credish_store *)arg;
    unsigned int seed = (unsigned int)(uintptr_t)s ^ (unsigned int)time(NULL);
    struct timespec interval = {
        .tv_sec  = 0,
        .tv_nsec = SWEEP_INTERVAL_MS * 1000000L,
    };
    while (s->sweep_running) {
        nanosleep(&interval, NULL);
        pthread_rwlock_wrlock(&s->lock);
        for (int i = 0; i < CREDISH_DB_COUNT; i++)
            sweep_db(&s->dbs[i], &seed);
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
