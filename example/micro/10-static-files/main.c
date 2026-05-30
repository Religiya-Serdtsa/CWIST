/**
 * @file main.c
 * @brief 10-static-files — serve a directory.
 */

#include <cwist/app.h>

int main(void) {
    cwist_app *app = cwist_app_create();
    /* Serve ./public at http://localhost:8080/static/ */
    cwist_app_static(app, "/static", "public");
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
