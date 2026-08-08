#include <cwist/app.h>

static void handle_quic(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "{\"status\":\"ok\",\"protocol\":\"HTTP/3 over QUIC\"}");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/", handle_quic);
    cwist_app_listen(app, 4433);
    cwist_app_destroy(app);
    return 0;
}
