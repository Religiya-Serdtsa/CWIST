/** @file pool.c @brief Bounded SQLite pool with O(1) lease bookkeeping. */
#define _POSIX_C_SOURCE 200809L
#include <cwist/core/db/pool.h>
#include <cwist/core/mem/alloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct cwist_db_pool {
    char *path;
    char *open_path;
    size_t max_conns, idle_count, in_use;
    cwist_db **conns;
    size_t *idle_slots;
    bool *leased;
    bool closing;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
};

static atomic_ulong pool_sequence = 1;

static cwist_error_t pool_error(void) { cwist_error_t err = make_error(CWIST_ERR_INT16); err.error.err_i16 = -1; return err; }

static bool pool_cond_init_monotonic(pthread_cond_t *cond) {
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) return false;
    bool ok = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 &&
              pthread_cond_init(cond, &attr) == 0;
    pthread_condattr_destroy(&attr);
    return ok;
}

static void pool_deadline_after_ms(struct timespec *deadline, int timeout_ms) {
    clock_gettime(CLOCK_MONOTONIC, deadline);
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
}

static char *pool_open_path(const char *path) {
    if (strcmp(path, ":memory:") != 0) return cwist_strdup(path);
    unsigned long seq = atomic_fetch_add_explicit(&pool_sequence, 1, memory_order_relaxed);
    char buffer[96];
    int written = snprintf(buffer, sizeof(buffer), "file:cwist-pool-%lu?mode=memory&cache=shared", seq);
    return written > 0 && (size_t)written < sizeof(buffer) ? cwist_strdup(buffer) : NULL;
}

cwist_db_pool_t *cwist_db_pool_create(const char *path, size_t max_conns) {
    if (!path || max_conns == 0) return NULL;
    cwist_db_pool_t *pool = cwist_alloc(sizeof(*pool));
    if (!pool) return NULL;
    memset(pool, 0, sizeof(*pool));
    pool->path = cwist_strdup(path);
    pool->open_path = pool_open_path(path);
    pool->conns = cwist_alloc_array(max_conns, sizeof(*pool->conns));
    pool->idle_slots = cwist_alloc_array(max_conns, sizeof(*pool->idle_slots));
    pool->leased = cwist_alloc_array(max_conns, sizeof(*pool->leased));
    bool mutex_ready = false;
    bool cond_ready = false;
    if (!pool->path || !pool->open_path || !pool->conns || !pool->idle_slots || !pool->leased) goto fail;
    if (pthread_mutex_init(&pool->mtx, NULL) != 0) goto fail;
    mutex_ready = true;
    if (!pool_cond_init_monotonic(&pool->cond)) goto fail;
    cond_ready = true;
    pool->max_conns = max_conns;
    for (size_t i = 0; i < max_conns; ++i) {
        if (cwist_db_open(&pool->conns[i], pool->open_path).error.err_i16 != 0) goto fail;
        pool->conns[i]->pool_slot = i;
        sqlite3_busy_timeout(pool->conns[i]->conn, 5000);
        pool->idle_slots[pool->idle_count++] = i;
    }
    return pool;
fail:
    if (pool->conns) for (size_t i = 0; i < max_conns; ++i) cwist_db_close(pool->conns[i]);
    if (cond_ready) pthread_cond_destroy(&pool->cond);
    if (mutex_ready) pthread_mutex_destroy(&pool->mtx);
    cwist_free(pool->leased); cwist_free(pool->idle_slots); cwist_free(pool->conns); cwist_free(pool->open_path); cwist_free(pool->path); cwist_free(pool);
    return NULL;
}

static cwist_db *pool_acquire_locked(cwist_db_pool_t *pool) {
    if (pool->closing || pool->idle_count == 0) return NULL;
    size_t slot = pool->idle_slots[--pool->idle_count];
    pool->leased[slot] = true;
    ++pool->in_use;
    return pool->conns[slot];
}

cwist_db *cwist_db_pool_acquire(cwist_db_pool_t *pool) {
    if (!pool) return NULL;
    pthread_mutex_lock(&pool->mtx);
    while (!pool->closing && pool->idle_count == 0) pthread_cond_wait(&pool->cond, &pool->mtx);
    cwist_db *db = pool_acquire_locked(pool);
    pthread_mutex_unlock(&pool->mtx);
    return db;
}

cwist_db *cwist_db_pool_acquire_timeout(cwist_db_pool_t *pool, int timeout_ms) {
    if (!pool || timeout_ms < 0) return NULL;
    struct timespec deadline;
    pool_deadline_after_ms(&deadline, timeout_ms);
    pthread_mutex_lock(&pool->mtx);
    while (!pool->closing && pool->idle_count == 0) {
        if (pthread_cond_timedwait(&pool->cond, &pool->mtx, &deadline) != 0) break;
    }
    cwist_db *db = pool_acquire_locked(pool);
    pthread_mutex_unlock(&pool->mtx);
    return db;
}

void cwist_db_pool_release(cwist_db_pool_t *pool, cwist_db *conn) {
    if (!pool || !conn) return;
    pthread_mutex_lock(&pool->mtx);
    size_t slot = conn->pool_slot;
    if (slot < pool->max_conns && pool->conns[slot] == conn && pool->leased[slot]) {
        pool->leased[slot] = false;
        pool->idle_slots[pool->idle_count++] = slot;
        --pool->in_use;
        pthread_cond_broadcast(&pool->cond);
    }
    pthread_mutex_unlock(&pool->mtx);
}

size_t cwist_db_pool_in_use(cwist_db_pool_t *pool) {
    if (!pool) return 0;
    pthread_mutex_lock(&pool->mtx); size_t count = pool->in_use; pthread_mutex_unlock(&pool->mtx);
    return count;
}

bool cwist_db_pool_destroy_timeout(cwist_db_pool_t *pool, int timeout_ms) {
    if (!pool || timeout_ms < -1) return false;
    pthread_mutex_lock(&pool->mtx);
    pool->closing = true;
    pthread_cond_broadcast(&pool->cond);
    if (timeout_ms < 0) {
        while (pool->in_use != 0) pthread_cond_wait(&pool->cond, &pool->mtx);
    } else {
        struct timespec deadline;
        pool_deadline_after_ms(&deadline, timeout_ms);
        while (pool->in_use != 0) {
            int rc = pthread_cond_timedwait(&pool->cond, &pool->mtx, &deadline);
            if (rc != 0 && pool->in_use != 0) {
                pthread_mutex_unlock(&pool->mtx);
                return false;
            }
        }
    }
    pthread_mutex_unlock(&pool->mtx);
    for (size_t i = 0; i < pool->max_conns; ++i) cwist_db_close(pool->conns[i]);
    pthread_cond_destroy(&pool->cond); pthread_mutex_destroy(&pool->mtx);
    cwist_free(pool->leased); cwist_free(pool->idle_slots); cwist_free(pool->conns); cwist_free(pool->open_path); cwist_free(pool->path); cwist_free(pool);
    return true;
}

void cwist_db_pool_destroy(cwist_db_pool_t *pool) {
    (void)cwist_db_pool_destroy_timeout(pool, -1);
}

cwist_error_t cwist_db_pool_exec(cwist_db_pool_t *pool, const char *sql) {
    cwist_db *db = cwist_db_pool_acquire(pool); if (!db) return pool_error();
    cwist_error_t err = cwist_db_exec(db, sql); cwist_db_pool_release(pool, db); return err;
}

cwist_error_t cwist_db_pool_query(cwist_db_pool_t *pool, const char *sql, cJSON **result) {
    cwist_db *db = cwist_db_pool_acquire(pool); if (!db) return pool_error();
    cwist_error_t err = cwist_db_query(db, sql, result); cwist_db_pool_release(pool, db); return err;
}
