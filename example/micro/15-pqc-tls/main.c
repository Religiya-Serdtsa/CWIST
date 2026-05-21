/**
 * @file main.c
 * @brief 15-pqc-tls — one-line Post-Quantum TLS.
 */

#include <cwist/app.h>

static void hello(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "Quantum-safe!");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use_https(app, "server.crt", "server.key");
    cwist_app_use_pqc_layer(app, true);   /* X25519MLKEM768 */
    cwist_app_get(app, "/", hello);
    cwist_app_listen(app, 8443);
    cwist_app_destroy(app);
    return 0;
}
