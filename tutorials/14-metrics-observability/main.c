#include <cwist/app.h>

static void handle_api(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "{\"status\":\"ok\",\"service\":\"monitored_api\"}");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_enable_metrics(app);
    cwist_app_get(app, "/api/data", handle_api);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
