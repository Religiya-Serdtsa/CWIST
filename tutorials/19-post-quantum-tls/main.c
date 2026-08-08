#include <cwist/app.h>

static void handle_secure(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "{\"status\":\"ok\",\"security\":\"Post-Quantum TLS Active\"}");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use_pqc_layer(app, true);
    cwist_app_get(app, "/", handle_secure);
    cwist_app_listen(app, 8443);
    cwist_app_destroy(app);
    return 0;
}
