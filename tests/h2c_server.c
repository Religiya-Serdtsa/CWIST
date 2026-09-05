/**
 * @file h2c_server.c
 * @brief Minimal cleartext HTTP/2 (h2c) server for h2spec conformance runs.
 * Not part of the test suite; used by the interop CI job.
 */

#include <cwist/app.h>
#include <cwist/sys/app/app.h>
#include <stdio.h>
#include <stdlib.h>

static void hello(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "Hello, World!");
}

static void echo(cwist_http_request *req, cwist_http_response *res) {
    if (req->body && req->body->data) {
        cwist_sstring_assign(res->body, req->body->data);
    } else {
        cwist_sstring_assign(res->body, "");
    }
}

int main(int argc, char **argv) {
    int port = (argc > 1) ? atoi(argv[1]) : 8080;
    cwist_app *app = cwist_app_create();
    if (!app) return 1;
    if (cwist_app_use_http2(app, true).error.err_i16 != 0) {
        fprintf(stderr, "failed to enable h2c\n");
        return 1;
    }
    cwist_app_get(app, "/", hello);
    cwist_app_post(app, "/", echo);
    fprintf(stderr, "h2c conformance server on :%d\n", port);
    cwist_app_listen(app, port);
    cwist_app_destroy(app);
    return 0;
}
