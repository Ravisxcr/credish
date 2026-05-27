#ifndef CREDISH_PLATFORM_H
#define CREDISH_PLATFORM_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <process.h>
#include <windows.h>
#define credish_strcasecmp _stricmp

typedef SRWLOCK credish_rwlock_t;
typedef CRITICAL_SECTION credish_mutex_t;
typedef INIT_ONCE credish_once_t;
typedef HANDLE credish_thread_t;
#define CREDISH_ONCE_INIT INIT_ONCE_STATIC_INIT

typedef struct credish_thread_start {
    void *(*fn)(void *);
    void *arg;
} credish_thread_start;

static inline int credish_rwlock_init(credish_rwlock_t *lock) {
    InitializeSRWLock(lock);
    return 0;
}
static inline int credish_rwlock_rdlock(credish_rwlock_t *lock) {
    AcquireSRWLockShared(lock);
    return 0;
}
static inline int credish_rwlock_wrlock(credish_rwlock_t *lock) {
    AcquireSRWLockExclusive(lock);
    return 0;
}
static inline int credish_rwlock_rdunlock(credish_rwlock_t *lock) {
    ReleaseSRWLockShared(lock);
    return 0;
}
static inline int credish_rwlock_wrunlock(credish_rwlock_t *lock) {
    ReleaseSRWLockExclusive(lock);
    return 0;
}
static inline int credish_rwlock_destroy(credish_rwlock_t *lock) {
    (void)lock;
    return 0;
}

static inline int credish_mutex_init(credish_mutex_t *lock) {
    InitializeCriticalSection(lock);
    return 0;
}
static inline int credish_mutex_lock(credish_mutex_t *lock) {
    EnterCriticalSection(lock);
    return 0;
}
static inline int credish_mutex_unlock(credish_mutex_t *lock) {
    LeaveCriticalSection(lock);
    return 0;
}
static inline int credish_mutex_destroy(credish_mutex_t *lock) {
    DeleteCriticalSection(lock);
    return 0;
}

static BOOL CALLBACK credish_once_adapter(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    void (*fn)(void) = (void (*)(void))param;
    (void)once;
    (void)ctx;
    fn();
    return TRUE;
}
static inline int credish_once(credish_once_t *once, void (*fn)(void)) {
    return InitOnceExecuteOnce(once, credish_once_adapter, (PVOID)fn, NULL) ? 0 : -1;
}

static unsigned __stdcall credish_thread_adapter(void *arg) {
    credish_thread_start *start = (credish_thread_start *)arg;
    void *(*fn)(void *) = start->fn;
    void *fn_arg = start->arg;
    free(start);
    fn(fn_arg);
    return 0;
}
static inline int credish_thread_create(credish_thread_t *thread, void *(*fn)(void *), void *arg) {
    credish_thread_start *start = (credish_thread_start *)malloc(sizeof(*start));
    if (!start) return -1;
    start->fn = fn;
    start->arg = arg;
    *thread = (HANDLE)_beginthreadex(NULL, 0, credish_thread_adapter, start, 0, NULL);
    if (!*thread) {
        free(start);
        return -1;
    }
    return 0;
}
static inline int credish_thread_join(credish_thread_t thread) {
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return 0;
}
static inline int credish_thread_detach(credish_thread_t thread) {
    CloseHandle(thread);
    return 0;
}

static inline int64_t credish_now_ms(void) {
    FILETIME ft;
    ULARGE_INTEGER uli;
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return (int64_t)((uli.QuadPart - 116444736000000000ULL) / 10000ULL);
}
static inline void credish_sleep_ms(unsigned int ms) {
    Sleep(ms);
}

static inline int credish_fsync_file(FILE *f) {
    if (fflush(f) != 0) return -1;
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
    if (h == INVALID_HANDLE_VALUE) return -1;
    return FlushFileBuffers(h) ? 0 : -1;
}

static inline void credish_fsync_parent_dir(const char *path) {
    (void)path;
}

#else
#include <fcntl.h>
#include <pthread.h>
#include <strings.h>
#include <unistd.h>
#define credish_strcasecmp strcasecmp

typedef pthread_rwlock_t credish_rwlock_t;
typedef pthread_mutex_t credish_mutex_t;
typedef pthread_once_t credish_once_t;
typedef pthread_t credish_thread_t;
#define CREDISH_ONCE_INIT PTHREAD_ONCE_INIT
#define credish_rwlock_init(lock) pthread_rwlock_init((lock), NULL)
#define credish_rwlock_rdlock(lock) pthread_rwlock_rdlock(lock)
#define credish_rwlock_wrlock(lock) pthread_rwlock_wrlock(lock)
#define credish_rwlock_rdunlock(lock) pthread_rwlock_unlock(lock)
#define credish_rwlock_wrunlock(lock) pthread_rwlock_unlock(lock)
#define credish_rwlock_destroy(lock) pthread_rwlock_destroy(lock)
#define credish_mutex_init(lock) pthread_mutex_init((lock), NULL)
#define credish_mutex_lock(lock) pthread_mutex_lock(lock)
#define credish_mutex_unlock(lock) pthread_mutex_unlock(lock)
#define credish_mutex_destroy(lock) pthread_mutex_destroy(lock)
#define credish_once(once, fn) pthread_once((once), (fn))
#define credish_thread_create(thread, fn, arg) pthread_create((thread), NULL, (fn), (arg))
#define credish_thread_join(thread) pthread_join((thread), NULL)
#define credish_thread_detach(thread) pthread_detach(thread)

static inline int64_t credish_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}
static inline void credish_sleep_ms(unsigned int ms) {
    struct timespec interval = {
        .tv_sec = (time_t)(ms / 1000U),
        .tv_nsec = (long)(ms % 1000U) * 1000000L,
    };
    nanosleep(&interval, NULL);
}

static inline int credish_fsync_file(FILE *f) {
    if (fflush(f) != 0) return -1;
    return fsync(fileno(f));
}

static inline void credish_fsync_parent_dir(const char *path) {
    char dir[600];
    const char *slash = strrchr(path, '/');
    if (!slash) return;
    size_t len = (size_t)(slash - path);
    if (len == 0 || len >= sizeof(dir)) return;
    memcpy(dir, path, len);
    dir[len] = '\0';
    int fd = open(dir, O_RDONLY);
    if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
    }
}
#endif

static inline unsigned int credish_rand_r(unsigned int *seed) {
    *seed = *seed * 1103515245U + 12345U;
    return (*seed / 65536U) % 32768U;
}

#endif /* CREDISH_PLATFORM_H */
