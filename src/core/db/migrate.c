#include <cwist/core/db/migrate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file migrate.c
 * @brief Schema migration helpers for applying and rolling back ordered SQL revisions.
 */

/**
 * @brief Create the migration history table when the target database lacks it.
 * @param db SQLite connection to prepare for migration tracking.
 * @return CWIST_MIGRATE_OK on success, or a migration SQL error code on failure.
 */
static int migrate_ensure_table(sqlite3 *db) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS _cwist_migrations ("
        "  version   INTEGER PRIMARY KEY,"
        "  name      TEXT    NOT NULL,"
        "  applied_at TEXT   NOT NULL DEFAULT (datetime('now','utc'))"
        ");";
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[migrate] ensure table: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        return CWIST_MIGRATE_ERR_SQL;
    }
    return CWIST_MIGRATE_OK;
}

/**
 * @brief Execute a migration SQL blob inside an explicit transaction.
 * @param db SQLite connection to mutate.
 * @param sql SQL text for the up/down migration step.
 * @param version Migration version being applied.
 * @param direction Human-readable direction label used in logs.
 * @return CWIST_MIGRATE_OK on success, or a migration SQL error code on failure.
 */
static int migrate_exec_sql(sqlite3 *db, const char *sql, int version,
                             const char *direction) {
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, "BEGIN;", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[migrate] BEGIN failed (v%d %s): %s\n",
                version, direction, errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        return CWIST_MIGRATE_ERR_SQL;
    }

    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[migrate] SQL failed (v%d %s): %s\n",
                version, direction, errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return CWIST_MIGRATE_ERR_SQL;
    }

    rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[migrate] COMMIT failed (v%d %s): %s\n",
                version, direction, errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return CWIST_MIGRATE_ERR_SQL;
    }
    return CWIST_MIGRATE_OK;
}

/**
 * @brief qsort comparator that orders migrations by ascending version.
 * @param a Left migration pointer.
 * @param b Right migration pointer.
 * @return Negative, zero, or positive depending on version ordering.
 */
static int migration_cmp_asc(const void *a, const void *b) {
    const cwist_migration_t *ma = (const cwist_migration_t *)a;
    const cwist_migration_t *mb = (const cwist_migration_t *)b;
    return ma->version - mb->version;
}

/**
 * @brief qsort comparator that orders migrations by descending version.
 * @param a Left migration pointer.
 * @param b Right migration pointer.
 * @return Negative, zero, or positive depending on reverse version ordering.
 */
static int migration_cmp_desc(const void *a, const void *b) {
    const cwist_migration_t *ma = (const cwist_migration_t *)a;
    const cwist_migration_t *mb = (const cwist_migration_t *)b;
    return mb->version - ma->version;
}

/**
 * @brief Query the highest applied migration version recorded in the database.
 * @param db SQLite connection to inspect.
 * @return Highest applied version, or -1 when the query fails.
 */
int cwist_migrate_version(sqlite3 *db) {
    if (!db) return -1;
    if (migrate_ensure_table(db) != CWIST_MIGRATE_OK) return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT COALESCE(MAX(version), 0) FROM _cwist_migrations;",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    int version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return version;
}

/**
 * @brief Apply all pending up migrations after sorting them by version.
 * @param db SQLite connection to migrate.
 * @param migrations Caller-supplied migration array.
 * @param count Number of entries in @p migrations.
 * @return CWIST_MIGRATE_OK on success, or a specific migration error code.
 */
int cwist_migrate_up(sqlite3 *db, const cwist_migration_t *migrations,
                     int count) {
    if (!db || !migrations || count <= 0) return CWIST_MIGRATE_ERR_ARGS;

    if (migrate_ensure_table(db) != CWIST_MIGRATE_OK)
        return CWIST_MIGRATE_ERR_SQL;

    int current = cwist_migrate_version(db);
    if (current < 0) return CWIST_MIGRATE_ERR_SQL;

    /* Work on a sorted copy so callers need not pre-sort. */
    cwist_migration_t *sorted = (cwist_migration_t *)malloc(
        sizeof(cwist_migration_t) * (size_t)count);
    if (!sorted) return CWIST_MIGRATE_ERR_GENERIC;
    memcpy(sorted, migrations, sizeof(cwist_migration_t) * (size_t)count);
    qsort(sorted, (size_t)count, sizeof(cwist_migration_t), migration_cmp_asc);

    for (int i = 0; i < count; i++) {
        if (sorted[i].version <= current) continue;
        if (!sorted[i].up_sql) continue;

        int rc = migrate_exec_sql(db, sorted[i].up_sql,
                                  sorted[i].version, "up");
        if (rc != CWIST_MIGRATE_OK) {
            free(sorted);
            return rc;
        }

        /* Record in history. */
        {
            sqlite3_stmt *ins = NULL;
            if (sqlite3_prepare_v2(db,
                    "INSERT INTO _cwist_migrations (version, name) VALUES (?, ?);",
                    -1, &ins, NULL) == SQLITE_OK) {
                sqlite3_bind_int(ins, 1, sorted[i].version);
                sqlite3_bind_text(ins, 2,
                                  sorted[i].name ? sorted[i].name : "",
                                  -1, SQLITE_STATIC);
                sqlite3_step(ins);
                sqlite3_finalize(ins);
            }
        }
    }

    free(sorted);
    return CWIST_MIGRATE_OK;
}

/**
 * @brief Roll back applied migrations in reverse version order.
 * @param db SQLite connection to migrate.
 * @param migrations Caller-supplied migration array.
 * @param count Number of entries in @p migrations.
 * @param steps Maximum number of applied versions to roll back, or all when <= 0.
 * @return CWIST_MIGRATE_OK on success, or a specific migration error code.
 */
int cwist_migrate_down(sqlite3 *db, const cwist_migration_t *migrations,
                       int count, int steps) {
    if (!db || !migrations || count <= 0) return CWIST_MIGRATE_ERR_ARGS;

    if (migrate_ensure_table(db) != CWIST_MIGRATE_OK)
        return CWIST_MIGRATE_ERR_SQL;

    int current = cwist_migrate_version(db);
    if (current <= 0) return CWIST_MIGRATE_OK; /* nothing to roll back */

    /* Sort descending so we undo in reverse order. */
    cwist_migration_t *sorted = (cwist_migration_t *)malloc(
        sizeof(cwist_migration_t) * (size_t)count);
    if (!sorted) return CWIST_MIGRATE_ERR_GENERIC;
    memcpy(sorted, migrations, sizeof(cwist_migration_t) * (size_t)count);
    qsort(sorted, (size_t)count, sizeof(cwist_migration_t), migration_cmp_desc);

    int rolled = 0;
    int limit = (steps <= 0) ? count : steps;

    for (int i = 0; i < count && rolled < limit; i++) {
        if (sorted[i].version > current) continue;

        /* Check it's actually in the history table. */
        sqlite3_stmt *chk = NULL;
        int found = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT 1 FROM _cwist_migrations WHERE version = ?;",
                -1, &chk, NULL) == SQLITE_OK) {
            sqlite3_bind_int(chk, 1, sorted[i].version);
            if (sqlite3_step(chk) == SQLITE_ROW) found = 1;
            sqlite3_finalize(chk);
        }
        if (!found) continue;

        if (sorted[i].down_sql) {
            int rc = migrate_exec_sql(db, sorted[i].down_sql,
                                      sorted[i].version, "down");
            if (rc != CWIST_MIGRATE_OK) {
                free(sorted);
                return rc;
            }
        }

        /* Remove from history. */
        {
            sqlite3_stmt *del = NULL;
            if (sqlite3_prepare_v2(db,
                    "DELETE FROM _cwist_migrations WHERE version = ?;",
                    -1, &del, NULL) == SQLITE_OK) {
                sqlite3_bind_int(del, 1, sorted[i].version);
                sqlite3_step(del);
                sqlite3_finalize(del);
            }
        }

        rolled++;
    }

    free(sorted);
    return CWIST_MIGRATE_OK;
}
