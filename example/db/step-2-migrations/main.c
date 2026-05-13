/**
 * @file main.c
 * @brief Schema migrations via the app-level DB API.
 */

#include <stdio.h>
#include <cwist/app.h>
#include <cwist/core/db/sql.h>
#include <cwist/core/db/migrate.h>

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
        NULL
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

int main(void) {
    printf("=== Schema Migrations Tutorial ===\n");

    cwist_app *app = cwist_app_create();
    cwist_app_use_db(app, ":memory:");
    cwist_db *db = cwist_app_get_db(app);

    printf("\n[Migrate UP -- apply all]\n");
    int rc = cwist_migrate_up(db->conn, migrations, N);
    if (rc != CWIST_MIGRATE_OK) {
        fprintf(stderr, "Migration up failed: %d\n", rc);
        cwist_app_destroy(app);
        return 1;
    }
    printf("Current version: %d\n", cwist_migrate_version(db->conn));

    printf("\n[Migrate DOWN -- roll back 1 step]\n");
    rc = cwist_migrate_down(db->conn, migrations, N, 1);
    if (rc != CWIST_MIGRATE_OK) {
        fprintf(stderr, "Migration down failed: %d\n", rc);
        cwist_app_destroy(app);
        return 1;
    }
    printf("Current version after rollback: %d\n", cwist_migrate_version(db->conn));

    printf("\n[Migrate UP -- re-apply]\n");
    cwist_migrate_up(db->conn, migrations, N);
    printf("Current version: %d\n", cwist_migrate_version(db->conn));

    cwist_app_destroy(app);
    printf("\n=== Done ===\n");
    return 0;
}
