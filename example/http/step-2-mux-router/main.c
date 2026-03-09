#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/mux.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/mem/alloc.h>

#define PORT 8081

/* ---- Route handlers ---- */
static void handle_home(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->status_text, "OK");
    cwist_http_header_add(&res->headers, "Content-Type", "text/html");
    cwist_sstring_assign(res->body,
        "<h1>Cwist Mux Router</h1>"
        "<ul>"
        "<li><a href='/hello'>GET /hello</a></li>"
        "<li>POST /echo  (send any body)</li>"
        "</ul>");
}

static void handle_hello(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->status_text, "OK");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    cwist_sstring_assign(res->body, "Hello from the mux router!");
}

static void handle_echo(cwist_http_request *req, cwist_http_response *res) {
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->status_text, "OK");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    cwist_sstring_assign(res->body, req->body ? req->body->data : "");
}

/* ---- Server glue ---- */
typedef struct { cwist_mux_router *router; } ctx_t;

static void handle_client(int client_fd, void *arg) {
    ctx_t *ctx = (ctx_t *)arg;
    char *buf  = cwist_alloc(CWIST_HTTP_READ_BUFFER_SIZE);
    if (!buf) { close(client_fd); return; }

    size_t buf_len = 0;
    cwist_http_request *req =
        cwist_http_receive_request(client_fd, buf, CWIST_HTTP_READ_BUFFER_SIZE, &buf_len);
    if (!req) { cwist_free(buf); close(client_fd); return; }

    printf("[%s] %s\n", cwist_http_method_to_string(req->method), req->path->data);

    cwist_http_response *res = cwist_http_response_create();
    cwist_http_header_add(&res->headers, "Connection", "close");
    res->keep_alive = false;

    if (!cwist_mux_serve(ctx->router, req, res)) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->status_text, "Not Found");
        cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
        cwist_sstring_assign(res->body, "404 - Not Found");
    }

    cwist_http_send_response(client_fd, res);
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    cwist_free(buf);
    close(client_fd);
}

int main() {
    cwist_mux_router *router = cwist_mux_router_create();
    cwist_mux_handle(router, CWIST_HTTP_GET,  "/",     handle_home);
    cwist_mux_handle(router, CWIST_HTTP_GET,  "/hello",handle_hello);
    cwist_mux_handle(router, CWIST_HTTP_POST, "/echo", handle_echo);

    struct sockaddr_in addr;
    int server_fd = cwist_make_socket_ipv4(&addr, "0.0.0.0", PORT, 128);
    if (server_fd < 0) {
        fprintf(stderr, "Failed to create socket (err=%d)\n", server_fd);
        cwist_mux_router_destroy(router);
        return 1;
    }

    printf("Listening on http://localhost:%d\n", PORT);
    printf("Routes: GET /   GET /hello   POST /echo\n");
    printf("Ctrl-C to stop.\n");

    ctx_t ctx = { .router = router };
    cwist_server_config cfg = {0};
    cfg.use_threading = true;
    cwist_http_server_loop(server_fd, &cfg, handle_client, &ctx);

    cwist_mux_router_destroy(router);
    return 0;
}
