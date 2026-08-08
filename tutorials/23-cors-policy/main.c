#include <cwist/app.h>
#include <cwist/sys/app/middleware.h>

static void handle_cors_api(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "{\"status\":\"ok\",\"message\":\"CORS enabled\"}");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use(app, cwist_mw_cors());
    cwist_app_get(app, "/api/cors", handle_cors_api);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
