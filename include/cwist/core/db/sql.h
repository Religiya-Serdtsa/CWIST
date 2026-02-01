#ifndef __CWIST_SQL_H__
#define __CWIST_SQL_H__

#include <sqlite3.h>
#include <cwist/sys/err/cwist_err.h>
#include <cjson/cJSON.h>

/**
 * Wrapper for Database Operations.
 * Currently uses SQLite3.
 */

typedef struct cwist_db {
    sqlite3 *conn;
} cwist_db;

/** @name API */

/**
 * Connect to a database (or open file).
 * path: Path to SQLite file (or ":memory:")
 */
cwist_error_t cwist_db_open(cwist_db **db, const char *path);

/**
 * Close database connection.
 */
void cwist_db_close(cwist_db *db);

/**
 * Execute a command (INSERT, UPDATE, DELETE, CREATE).
 * Does not return rows. Returns err_i16 = -1 when db/sql pointer is invalid.
 */
cwist_error_t cwist_db_exec(cwist_db *db, const char *sql);

/**
 * Execute a query and return results as a cJSON Array of Objects.
 * Example: [{"id":1, "name":"foo"}, {"id":2, "name":"bar"}]
 * The `result` pointer is reset to NULL on entry and left NULL on failure.
 */
cwist_error_t cwist_db_query(cwist_db *db, const char *sql, cJSON **result);

#endif
