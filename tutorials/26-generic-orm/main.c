#include <cwist/app.h>
#include <cwist/core/orm/orm.h>

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use_db(app, ":memory:");
    cwist_db *db = cwist_app_get_db(app);
    if (db) {
        printf("ORM SQLite database connected successfully.\n");
    }
    cwist_app_destroy(app);
    return 0;
}
