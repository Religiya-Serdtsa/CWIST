/**
 * @file orm.h
 * @brief Tiny ORM over a SQL socket stream.
 *
 * This header provides a socket-backed ORM interface.  It intentionally
 * never includes @c sqlite3.h; all communication with the SQL backend
 * happens through a Unix domain socket obtained from
 * cwist_db_transfer_sqlite_to_socket().
 *
 * The default transaction mode is manual (immediate commit disabled).
 * Call cwist_orm_immediate_commit(true) to make every write operation
 * automatically commit.
 */

#ifndef __CWIST_ORM_H__
#define __CWIST_ORM_H__

#include <stdbool.h>
#include <cwist/sys/err/cwist_err.h>
#include <cjson/cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration ----------------------------------------------------- */

/** @brief Opaque ORM session handle. */
typedef struct cwist_orm cwist_orm_t;

/** @brief Supported SQL dialects for query generation. */
typedef enum cwist_orm_dialect {
    CWIST_ORM_SQLITE,   /**< SQLite dialect (default) */
    CWIST_ORM_POSTGRES, /**< PostgreSQL dialect */
    CWIST_ORM_MYSQL,    /**< MySQL dialect */
    CWIST_ORM_MARIADB,  /**< MariaDB dialect */
} cwist_orm_dialect_t;

/* Lifecycle --------------------------------------------------------------- */

/**
 * @brief Create an ORM session from an existing SQL socket.
 *
 * The socket must have been produced by cwist_db_transfer_sqlite_to_socket()
 * or by an equivalent SQL-socket factory.  The ORM object takes ownership
 * of @p socket_fd and will close it on cwist_orm_close_socket().
 *
 * @param socket_fd Valid connected Unix socket file descriptor.
 * @return Pointer to an initialized cwist_orm_t, or @c NULL on allocation
 *         failure.
 */
cwist_orm_t *cwist_orm_open_socket(int socket_fd);

/**
 * @brief Destroy an ORM session and close its underlying socket.
 *
 * If a transaction is still open, it is rolled back before the socket
 * is closed.  The pointer is invalid after this call.
 *
 * @param orm ORM handle created with cwist_orm_open_socket().
 */
void cwist_orm_close_socket(cwist_orm_t *orm);

/* Transaction control ----------------------------------------------------- */

/**
 * @brief Toggle automatic commit mode.
 *
 * When enabled, every cwist_orm_insert(), cwist_orm_update(),
 * cwist_orm_delete(), and cwist_orm_exec() automatically appends a
 * @c COMMIT statement so that the change is persisted immediately.
 *
 * When disabled (the default), the caller is responsible for wrapping
 * operations in explicit transactions via cwist_orm_commit() or
 * cwist_orm_rollback().
 *
 * @param enable @c true to enable auto-commit, @c false to disable.
 */
void cwist_orm_immediate_commit(bool enable);

/**
 * @brief Select the SQL dialect used for query generation.
 *
 * The dialect affects identifier quoting, boolean literals, and
 * dialect-specific syntax in high-level helpers such as
 * cwist_orm_insert() and cwist_orm_update().
 *
 * @param orm     ORM session.
 * @param dialect Target dialect.  Default is CWIST_ORM_SQLITE.
 */
void cwist_orm_use_dialect(cwist_orm_t *orm, cwist_orm_dialect_t dialect);

/**
 * @brief Commit the current transaction.
 *
 * Has no effect when immediate commit is enabled.
 *
 * @param orm Active ORM session.
 * @return cwist_error_t with err_i16 == 0 on success, or
 *         CWIST_ERROR_IO on socket failure.
 */
cwist_error_t cwist_orm_commit(cwist_orm_t *orm);

/**
 * @brief Roll back the current transaction.
 *
 * Has no effect when immediate commit is enabled.
 *
 * @param orm Active ORM session.
 * @return cwist_error_t with err_i16 == 0 on success, or
 *         CWIST_ERROR_IO on socket failure.
 */
cwist_error_t cwist_orm_rollback(cwist_orm_t *orm);

/* Low-level SQL ----------------------------------------------------------- */

/**
 * @brief Execute a non-query SQL statement.
 *
 * Suitable for @c INSERT, @c UPDATE, @c DELETE, @c CREATE, @c DROP,
 * @c ALTER, @c BEGIN, @c COMMIT, @c ROLLBACK, and any other statement
 * that does not return result rows.
 *
 * When immediate commit mode is active and the statement is a write
 * operation, a @c COMMIT is automatically issued afterwards.
 *
 * @param orm ORM session.
 * @param sql Null-terminated SQL statement.
 * @return cwist_error_t with err_i16 == 0 on success, or a negative
 *         error code on failure (CWIST_ERROR_IO, CWIST_ERROR_PROTOCOL,
 *         etc.).
 */
cwist_error_t cwist_orm_exec(cwist_orm_t *orm, const char *sql);

/**
 * @brief Execute a SELECT query and return the result set.
 *
 * The returned cJSON object is an array.  Each element is an object
 * whose keys are column names and whose values are the row data.
 *
 * @param orm    ORM session.
 * @param sql    Null-terminated SELECT statement.
 * @param result [out] Pointer to a cJSON pointer.  On success it receives
 *               a cJSON array that the caller must free with
 *               cJSON_Delete().  On failure the pointer is set to
 *               @c NULL.
 * @return cwist_error_t with err_i16 == 0 on success.
 */
cwist_error_t cwist_orm_query(cwist_orm_t *orm, const char *sql,
                              cJSON **result);

/* High-level helpers ------------------------------------------------------ */

/**
 * @brief Insert a JSON object into a table.
 *
 * Scans @p data for keys and values, builds an @c INSERT statement, and
 * executes it.  Nested objects or arrays inside @p data are stringified
 * with cJSON_PrintUnformatted().
 *
 * Example:
 * @code
 * cJSON *row = cJSON_Parse("{\"name\":\"Alice\",\"age\":30}");
 * cwist_orm_insert(orm, "users", row);
 * @endcode
 *
 * @param orm   ORM session.
 * @param table Target table name (used verbatim; must be sanitized by
 *              the caller if it comes from untrusted input).
 * @param data  cJSON object (not array) containing the row data.
 * @return cwist_error_t with err_i16 == 0 on success.
 */
cwist_error_t cwist_orm_insert(cwist_orm_t *orm, const char *table,
                               const cJSON *data);

/**
 * @brief Update rows in a table.
 *
 * Builds an @c UPDATE statement from the key-value pairs in @p data.
 * If @p where_clause is non-NULL it is appended as the @c WHERE clause.
 *
 * @param orm          ORM session.
 * @param table        Target table name.
 * @param data         cJSON object with columns to update.
 * @param where_clause Optional SQL predicate (without the leading
 *                     @c WHERE keyword), or @c NULL to update all rows.
 * @return cwist_error_t with err_i16 == 0 on success.
 */
cwist_error_t cwist_orm_update(cwist_orm_t *orm, const char *table,
                               const cJSON *data,
                               const char *where_clause);

/**
 * @brief Delete rows from a table.
 *
 * @param orm          ORM session.
 * @param table        Target table name.
 * @param where_clause Optional SQL predicate (without @c WHERE), or
 *                     @c NULL to delete every row.
 * @return cwist_error_t with err_i16 == 0 on success.
 */
cwist_error_t cwist_orm_delete(cwist_orm_t *orm, const char *table,
                               const char *where_clause);

/**
 * @brief Convenience SELECT helper.
 *
 * Builds <tt>SELECT columns FROM table [WHERE where_clause]</tt> and
 * returns the parsed result set.
 *
 * @param orm          ORM session.
 * @param table        Target table name.
 * @param columns      Comma-separated column list or @c "*".
 * @param where_clause Optional predicate without @c WHERE, or @c NULL.
 * @param result       [out] cJSON array result; caller must free.
 * @return cwist_error_t with err_i16 == 0 on success.
 */
cwist_error_t cwist_orm_select(cwist_orm_t *orm, const char *table,
                               const char *columns,
                               const char *where_clause,
                               cJSON **result);

/* ------------------------------------------------------------------ */
/* _Generic type-dispatched RETURNING INSERT                           */
/* ------------------------------------------------------------------ */

cwist_error_t cwist_orm_insert_returning_json(cwist_orm_t *orm,
                                               const char *table,
                                               const cJSON *data,
                                               const char *returning_col,
                                               cJSON **out);

cwist_error_t cwist_orm_insert_returning_int(cwist_orm_t *orm,
                                              const char *table,
                                              const cJSON *data,
                                              const char *returning_col,
                                              int *out);

cwist_error_t cwist_orm_insert_returning_long(cwist_orm_t *orm,
                                               const char *table,
                                               const cJSON *data,
                                               const char *returning_col,
                                               long *out);

cwist_error_t cwist_orm_insert_returning_llong(cwist_orm_t *orm,
                                                const char *table,
                                                const cJSON *data,
                                                const char *returning_col,
                                                long long *out);

/**
 * @brief Type-generic INSERT with RETURNING clause.
 *
 * Dispatches to the appropriate typed helper based on the type of @p out.
 *
 * Example:
 * @code
 * int id;
 * cwist_orm_insert_r(orm, "users", row, "id", &id);
 *
 * cJSON *obj;
 * cwist_orm_insert_r(orm, "users", row, "*", &obj);
 * @endcode
 */
#define cwist_orm_insert_r(orm, table, data, returning_col, out) \
    _Generic((out), \
        int *        : cwist_orm_insert_returning_int, \
        long *       : cwist_orm_insert_returning_long, \
        long long *  : cwist_orm_insert_returning_llong, \
        cJSON **     : cwist_orm_insert_returning_json \
    )((orm), (table), (data), (returning_col), (out))

/* ------------------------------------------------------------------ */
/* _Generic type-dispatched SELECT single scalar                       */
/* ------------------------------------------------------------------ */

cwist_error_t cwist_orm_select_one_json(cwist_orm_t *orm,
                                        const char *table,
                                        const char *column,
                                        const char *where_clause,
                                        cJSON **out);

cwist_error_t cwist_orm_select_one_int(cwist_orm_t *orm,
                                       const char *table,
                                       const char *column,
                                       const char *where_clause,
                                       int *out);

cwist_error_t cwist_orm_select_one_long(cwist_orm_t *orm,
                                        const char *table,
                                        const char *column,
                                        const char *where_clause,
                                        long *out);

cwist_error_t cwist_orm_select_one_llong(cwist_orm_t *orm,
                                         const char *table,
                                         const char *column,
                                         const char *where_clause,
                                         long long *out);

cwist_error_t cwist_orm_select_one_double(cwist_orm_t *orm,
                                          const char *table,
                                          const char *column,
                                          const char *where_clause,
                                          double *out);

cwist_error_t cwist_orm_select_one_string(cwist_orm_t *orm,
                                          const char *table,
                                          const char *column,
                                          const char *where_clause,
                                          char **out);

/**
 * @brief Type-generic SELECT a single scalar value.
 *
 * Builds <tt>SELECT column FROM table [WHERE where_clause] LIMIT 1</tt>
 * and writes the first cell into @p out according to its C type.
 *
 * The caller must free a @c char* or @c cJSON* result.
 *
 * Example:
 * @code
 * int age;
 * cwist_orm_select_one(orm, "users", "age", "id = 1", &age);
 * @endcode
 */
#define cwist_orm_select_one(orm, table, column, where, out) \
    _Generic((out), \
        int *        : cwist_orm_select_one_int, \
        long *       : cwist_orm_select_one_long, \
        long long *  : cwist_orm_select_one_llong, \
        double *     : cwist_orm_select_one_double, \
        char **      : cwist_orm_select_one_string, \
        cJSON **     : cwist_orm_select_one_json \
    )((orm), (table), (column), (where), (out))

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_ORM_H__ */
