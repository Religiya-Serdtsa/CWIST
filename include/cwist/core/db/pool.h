/**
 * @file pool.h
 * @brief SQLite connection pool for CWIST.
 */

#ifndef __CWIST_DB_POOL_H__
#define __CWIST_DB_POOL_H__

#include <cwist/core/db/sql.h>
#include <cwist/sys/err/cwist_err.h>
#include <cjson/cJSON.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque database connection pool handle.
 */
typedef struct cwist_db_pool cwist_db_pool_t;

/**
 * @brief Create a pool of SQLite connections.
 *
 * @param path       SQLite database path (or ":memory:").
 * @param max_conns  Maximum number of connections (>= 1).
 * @return Pool handle, or NULL on failure.
 */
cwist_db_pool_t *cwist_db_pool_create(const char *path, size_t max_conns);

/**
 * @brief Destroy the pool and close all connections.
 */
void cwist_db_pool_destroy(cwist_db_pool_t *pool);

/**
 * @brief Acquire a connection from the pool. Blocks until one is available.
 *
 * @return SQLite db handle, or NULL on error.
 */
cwist_db *cwist_db_pool_acquire(cwist_db_pool_t *pool);

/**
 * @brief Return a connection to the pool.
 */
void cwist_db_pool_release(cwist_db_pool_t *pool, cwist_db *conn);

/**
 * @brief Execute a non-query using a pooled connection.
 */
cwist_error_t cwist_db_pool_exec(cwist_db_pool_t *pool, const char *sql);

/**
 * @brief Execute a query using a pooled connection.
 */
cwist_error_t cwist_db_pool_query(cwist_db_pool_t *pool, const char *sql, cJSON **result);

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_DB_POOL_H__ */
