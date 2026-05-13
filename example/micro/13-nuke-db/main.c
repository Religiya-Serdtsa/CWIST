/**
 * @file main.c
 * @brief 13-nuke-db — in-memory SQLite synced to disk.
 */

#include <cwist/app.h>
#include <cwist/core/db/sql.h>

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use_nuke_db(app, "blog.db", 5000); /* sync every 5s */

    cwist_db *db = cwist_app_get_db(app);
    cwist_db_exec(db, "CREATE TABLE IF NOT EXISTS posts (id INTEGER PRIMARY KEY, title TEXT);");
    cwist_db_exec(db, "INSERT INTO posts (title) VALUES ('Nuke DB Post');");

    printf("Nuke DB ready. Query with cwist_nuke_get_db()\n");
    cwist_app_destroy(app);
    return 0;
}
