/**
 * @file sql.h
 * @brief Database operations wrapper (SQLite3).
 */

#ifndef __CWIST_SQL_H__
#define __CWIST_SQL_H__

#include <sqlite3.h>
#include <cwist/err/cwist_err.h>
#include <cjson/cJSON.h>

typedef struct cwist_db {
    sqlite3 *conn;
} cwist_db;

/** @name Lifecycle */
/** @{ */

/**
 * @brief Connect to a database (or open file).
 * @param db Out parameter for the database handle.
 * @param path Path to SQLite file (or ":memory:").
 */
cwist_error_t cwist_db_open(cwist_db **db, const char *path);

/**
 * @brief Close database connection.
 */
void cwist_db_close(cwist_db *db);

/** @} */

/** @name Execution */
/** @{ */

/**
 * @brief Execute a command (INSERT, UPDATE, DELETE, CREATE).
 * Does not return rows.
 */
cwist_error_t cwist_db_exec(cwist_db *db, const char *sql);

/**
 * @brief Execute a query and return results as a cJSON Array of Objects.
 * Example: [{"id":1, "name":"foo"}, {"id":2, "name":"bar"}]
 */
cwist_error_t cwist_db_query(cwist_db *db, const char *sql, cJSON **result);

/** @} */

#endif
