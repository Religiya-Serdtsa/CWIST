#include <cwist/app.h>
#include <cwist/core/db/migrate.h>

static const cwist_migration_t migrations[] = {
    {
        .version = 1,
        .name = "create_users_table",
        .up_sql = "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);",
        .down_sql = "DROP TABLE users;"
    }
};

int main(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) == SQLITE_OK) {
        int res = cwist_migrate_up(db, migrations, 1);
        if (res == CWIST_MIGRATE_OK) {
            printf("Migration version 1 applied successfully.\n");
        }
        sqlite3_close(db);
    }
    return 0;
}
