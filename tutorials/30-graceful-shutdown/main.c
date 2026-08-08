#include <cwist/app.h>

static void handle_home(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "{\"status\":\"ok\",\"message\":\"Graceful shutdown ready\"}");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/", handle_home);
    printf("Starting server. Send SIGTERM/SIGINT to initiate graceful shutdown.\n");
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
