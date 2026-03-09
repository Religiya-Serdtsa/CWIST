#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <cwist/net/http/http.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/mem/alloc.h>

#define PORT 8080

/* Minimal request handler: responds "Hello, World!" to every GET / */
static void handle_client(int client_fd, void *ctx) {
    (void)ctx;
    char *buf = cwist_alloc(CWIST_HTTP_READ_BUFFER_SIZE);
    if (!buf) { close(client_fd); return; }

    size_t buf_len = 0;
    cwist_http_request *req =
        cwist_http_receive_request(client_fd, buf, CWIST_HTTP_READ_BUFFER_SIZE, &buf_len);
    if (!req) { cwist_free(buf); close(client_fd); return; }

    printf("[%s] %s\n", cwist_http_method_to_string(req->method), req->path->data);

    cwist_http_response *res = cwist_http_response_create();
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->status_text, "OK");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    cwist_http_header_add(&res->headers, "Connection",   "close");
    cwist_sstring_assign(res->body, "Hello, World!");
    res->keep_alive = false;

    cwist_http_send_response(client_fd, res);
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    cwist_free(buf);
    close(client_fd);
}

int main() {
    struct sockaddr_in addr;
    int server_fd = cwist_make_socket_ipv4(&addr, "0.0.0.0", PORT, 128);
    if (server_fd < 0) {
        fprintf(stderr, "Failed to create socket (err=%d)\n", server_fd);
        return 1;
    }

    printf("Listening on http://localhost:%d\n", PORT);
    printf("Try: curl http://localhost:%d/\n", PORT);
    printf("Ctrl-C to stop.\n");

    cwist_server_config cfg = {0};
    cfg.use_threading = true;
    cwist_http_server_loop(server_fd, &cfg, handle_client, NULL);
    return 0;
}
