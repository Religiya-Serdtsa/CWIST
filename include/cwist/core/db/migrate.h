/**
 * @file migrate.h
 * @brief Schema migration system with version history and rollback.
 *
 * Each migration is a (version, name, up_sql, down_sql) tuple.
 * The system stores applied migrations in a `_cwist_migrations` table and
 * executes UP or DOWN SQL inside transactions, so a failed migration leaves
 * the database unchanged.
 */

#ifndef __CWIST_MIGRATE_H__
#define __CWIST_MIGRATE_H__

#include <sqlite3.h>

/** @brief Return codes. */
#define CWIST_MIGRATE_OK           0
#define CWIST_MIGRATE_ERR_GENERIC (-1)
#define CWIST_MIGRATE_ERR_SQL     (-2)
#define CWIST_MIGRATE_ERR_ARGS    (-3)

/**
 * @brief A single migration step.
 *
 * - version : monotonically increasing integer (e.g. 1, 2, 3 …)
 * - name    : human-readable label (used in history table)
 * - up_sql  : SQL executed when migrating forward; may contain multiple
 *             statements separated by ';'
 * - down_sql: SQL executed when rolling back; NULL means irreversible
 */
typedef struct cwist_migration_t {
    int         version;
    const char *name;
    const char *up_sql;
    const char *down_sql; /**< May be NULL for irreversible migrations. */
} cwist_migration_t;

/**
 * @brief Apply all migrations whose version > current schema version.
 *
 * Migrations are applied in ascending version order inside individual
 * transactions. On failure the transaction is rolled back and the function
 * returns the error code.
 *
 * @param db         Open SQLite3 handle.
 * @param migrations Array of migrations (need not be sorted; function sorts).
 * @param count      Number of elements in @p migrations.
 * @return CWIST_MIGRATE_OK on success, negative on failure.
 */
int cwist_migrate_up(sqlite3 *db, const cwist_migration_t *migrations, int count);

/**
 * @brief Roll back the most recent @p steps migrations.
 *
 * Runs each migration's down_sql (skips if NULL) inside a transaction and
 * removes the entry from the history table.
 *
 * @param db         Open SQLite3 handle.
 * @param migrations Array of migrations (same array passed to cwist_migrate_up).
 * @param count      Number of elements in @p migrations.
 * @param steps      Number of versions to roll back (0 = all applied).
 * @return CWIST_MIGRATE_OK on success, negative on failure.
 */
int cwist_migrate_down(sqlite3 *db, const cwist_migration_t *migrations,
                       int count, int steps);

/**
 * @brief Return the highest version number that has been applied.
 * @return Version number, or 0 if no migrations have been applied,
 *         or -1 on error.
 */
int cwist_migrate_version(sqlite3 *db);

#endif /* __CWIST_MIGRATE_H__ */
