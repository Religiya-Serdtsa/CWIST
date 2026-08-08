#include <cwist/app.h>

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_enable_healthz(app);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
