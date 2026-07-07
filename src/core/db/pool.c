/**
 * @file pool.c
 * @brief SQLite connection pool implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/core/db/pool.h>
#include <cwist/core/mem/alloc.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct cwist_db_pool {
    char *path;
    size_t max_conns;
    cwist_db **conns;
    bool *available;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
};

cwist_db_pool_t *cwist_db_pool_create(const char *path, size_t max_conns) {
    if (!path || max_conns == 0) return NULL;

    cwist_db_pool_t *pool = cwist_alloc(sizeof(*pool));
    if (!pool) return NULL;
    pool->path = cwist_strdup(path);
    pool->max_conns = max_conns;
    pool->conns = cwist_alloc_array(max_conns, sizeof(cwist_db *));
    pool->available = cwist_alloc_array(max_conns, sizeof(bool));
    if (!pool->path || !pool->conns || !pool->available) goto fail;

    for (size_t i = 0; i < max_conns; i++) {
        cwist_error_t dberr = cwist_db_open(&pool->conns[i], path);
        if (dberr.error.err_i16 != 0) goto fail;
        pool->available[i] = true;
    }

    pthread_mutex_init(&pool->mtx, NULL);
    pthread_cond_init(&pool->cond, NULL);
    return pool;

fail:
    if (pool) {
        for (size_t i = 0; i < max_conns && pool->conns; i++) {
            if (pool->conns[i]) cwist_db_close(pool->conns[i]);
        }
        cwist_free(pool->conns);
        cwist_free(pool->available);
        cwist_free(pool->path);
        cwist_free(pool);
    }
    return NULL;
}

void cwist_db_pool_destroy(cwist_db_pool_t *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->mtx);
    for (size_t i = 0; i < pool->max_conns; i++) {
        if (pool->conns[i]) cwist_db_close(pool->conns[i]);
    }
    pthread_mutex_unlock(&pool->mtx);
    pthread_mutex_destroy(&pool->mtx);
    pthread_cond_destroy(&pool->cond);
    cwist_free(pool->conns);
    cwist_free(pool->available);
    cwist_free(pool->path);
    cwist_free(pool);
}

cwist_db *cwist_db_pool_acquire(cwist_db_pool_t *pool) {
    if (!pool) return NULL;
    pthread_mutex_lock(&pool->mtx);
    while (1) {
        for (size_t i = 0; i < pool->max_conns; i++) {
            if (pool->available[i]) {
                pool->available[i] = false;
                pthread_mutex_unlock(&pool->mtx);
                return pool->conns[i];
            }
        }
        pthread_cond_wait(&pool->cond, &pool->mtx);
    }
}

void cwist_db_pool_release(cwist_db_pool_t *pool, cwist_db *conn) {
    if (!pool || !conn) return;
    pthread_mutex_lock(&pool->mtx);
    for (size_t i = 0; i < pool->max_conns; i++) {
        if (pool->conns[i] == conn) {
            pool->available[i] = true;
            pthread_cond_signal(&pool->cond);
            break;
        }
    }
    pthread_mutex_unlock(&pool->mtx);
}

cwist_error_t cwist_db_pool_exec(cwist_db_pool_t *pool, const char *sql) {
    cwist_db *db = cwist_db_pool_acquire(pool);
    if (!db) return make_error(CWIST_ERR_INT16);
    cwist_error_t err = cwist_db_exec(db, sql);
    cwist_db_pool_release(pool, db);
    return err;
}

cwist_error_t cwist_db_pool_query(cwist_db_pool_t *pool, const char *sql, cJSON **result) {
    cwist_db *db = cwist_db_pool_acquire(pool);
    if (!db) return make_error(CWIST_ERR_INT16);
    cwist_error_t err = cwist_db_query(db, sql, result);
    cwist_db_pool_release(pool, db);
    return err;
}
