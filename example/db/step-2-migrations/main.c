#include <stdio.h>
#include <sqlite3.h>
#include <cwist/core/db/migrate.h>

/* Define migrations as a flat array.
 * Each entry: version, name, up_sql, down_sql (NULL = irreversible). */
static const cwist_migration_t migrations[] = {
    {
        1,
        "create_users",
        "CREATE TABLE users ("
        "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL"
        ");",
        "DROP TABLE users;"
    },
    {
        2,
        "add_email_to_users",
        "ALTER TABLE users ADD COLUMN email TEXT;",
        NULL   /* SQLite cannot drop columns — irreversible */
    },
    {
        3,
        "create_posts",
        "CREATE TABLE posts ("
        "  id      INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  user_id INTEGER NOT NULL,"
        "  title   TEXT NOT NULL"
        ");",
        "DROP TABLE posts;"
    },
};

static const int N = (int)(sizeof(migrations) / sizeof(migrations[0]));

int main() {
    printf("=== Schema Migrations Tutorial ===\n");

    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        fprintf(stderr, "Failed to open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    /* 1. Apply all pending migrations */
    printf("\n[Migrate UP — apply all]\n");
    int rc = cwist_migrate_up(db, migrations, N);
    if (rc != CWIST_MIGRATE_OK) {
        fprintf(stderr, "Migration up failed: %d\n", rc);
        sqlite3_close(db);
        return 1;
    }
    printf("Current version: %d\n", cwist_migrate_version(db));

    /* 2. Roll back the last migration (posts) */
    printf("\n[Migrate DOWN — roll back 1 step]\n");
    rc = cwist_migrate_down(db, migrations, N, 1);
    if (rc != CWIST_MIGRATE_OK) {
        fprintf(stderr, "Migration down failed: %d\n", rc);
        sqlite3_close(db);
        return 1;
    }
    printf("Current version after rollback: %d\n", cwist_migrate_version(db));

    /* 3. Re-apply (migrate up again) */
    printf("\n[Migrate UP — re-apply]\n");
    cwist_migrate_up(db, migrations, N);
    printf("Current version: %d\n", cwist_migrate_version(db));

    sqlite3_close(db);
    printf("\n=== Done ===\n");
    return 0;
}
