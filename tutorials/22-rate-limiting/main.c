#include <cwist/app.h>
#include <cwist/middleware.h>

static void handle_api(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "{\"status\":\"ok\",\"message\":\"Request allowed under rate limit\"}");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use(app, cwist_mw_rate_limit_ip(60));
    cwist_app_get(app, "/api/resource", handle_api);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
