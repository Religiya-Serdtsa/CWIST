#ifndef __CWIST_SQL_H__
#define __CWIST_SQL_H__

#include <sqlite3.h>
#include <cwist/sys/err/cwist_err.h>
#include <cjson/cJSON.h>
#include <cwist/core/utils/json_heal.h>
#include <cwist/core/utils/zod.h>

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

/**
 * @brief Insert a (possibly broken) JSON object into a table with self-healing.
 *
 * Applies the self-healing layer to `json_str`, validates the result with the
 * Zod-like strict validator (if a schema is provided), then generates and
 * executes an INSERT SQL statement.
 *
 * Returns an error when:
 *  - the JSON cannot be healed to a confidence >= cfg->threshold
 *  - Zod validation rejects the healed object (missing required fields /
 *    wrong types)
 *  - the SQL execution fails
 *
 * @param db       Open database handle.
 * @param table    Target table name (used verbatim in the SQL).
 * @param json_str Raw (possibly malformed) JSON string.
 * @param schema   Schema used for L2 healing and strict validation (may be NULL).
 * @param heal_cfg Healing configuration (may be NULL for defaults).
 * @return         cwist_error_t; err_i16 == 0 on success.
 */
cwist_error_t cwist_db_insert_healed(cwist_db *db, const char *table,
                                      const char *json_str,
                                      const cwist_schema_t  *schema,
                                      const cwist_heal_config_t *heal_cfg);

/**
 * @brief Execute a SELECT query and strictly validate each result row.
 *
 * Rows that fail Zod validation are excluded from the returned array.
 * When schema is NULL the function behaves identically to cwist_db_query().
 *
 * @param db     Open database handle.
 * @param sql    SQL SELECT statement.
 * @param result [out] cJSON array containing only conforming rows.
 * @param schema Schema to validate each row against (may be NULL).
 * @return       cwist_error_t; err_i16 == 0 on success.
 */
cwist_error_t cwist_db_query_strict(cwist_db *db, const char *sql, cJSON **result,
                                     const cwist_schema_t *schema);

#endif
