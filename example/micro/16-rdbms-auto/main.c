/**
 * @file main.c
 * @brief 16-rdbms-auto — probe localhost for PostgreSQL / MySQL / MariaDB.
 */

#include <cwist/app.h>
#include <stdio.h>

int main(void) {
    cwist_app *app = cwist_app_create();
    if (cwist_app_auto_rdbms(app, 5432)) {
        printf("RDBMS mounted on port 5432\n");
    } else {
        printf("No RDBMS detected on port 5432\n");
    }
    cwist_app_destroy(app);
    return 0;
}
