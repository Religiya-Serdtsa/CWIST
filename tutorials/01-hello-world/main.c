#include <cwist/app.h>

static void handle_hello(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "Hello, CWIST Web Framework!");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/", handle_hello);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
