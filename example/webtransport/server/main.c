#define _POSIX_C_SOURCE 200809L

#include <cwist/net/http/http3.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static cwist_http3_context *g_ctx;

static void on_signal(int signo) {
    (void)signo;
    if (g_ctx) g_ctx->running = 0;
}

static void http_fallback(void *user_ctx,
                          cwist_http_request *req,
                          cwist_http_response *res) {
    (void)user_ctx;
    (void)req;
    res->status_code = CWIST_HTTP_OK;
    cwist_http_header_add(&res->headers, "content-type", "text/plain; charset=utf-8");
    cwist_sstring_assign(res->body, "CWIST HTTP/3 is running. WebTransport endpoint: /wt\n");
}

static void on_datagram(const void *data, size_t len, void *user_ctx) {
    (void)user_ctx;
    printf("[wt] datagram %zu bytes: %.*s\n", len, (int)len, (const char *)data);
}

static void on_wt_stream(void *stream, void *user_ctx) {
    (void)user_ctx;

    char buf[2048];
    ssize_t nread = cwist_webtransport_read(stream, buf, sizeof(buf));
    if (nread <= 0) {
        return;
    }

    printf("[wt] stream %zd bytes: %.*s\n", nread, (int)nread, buf);
    const char prefix[] = "echo: ";
    cwist_webtransport_write(stream, prefix, sizeof(prefix) - 1);
    cwist_webtransport_write(stream, buf, (size_t)nread);
    cwist_webtransport_flush(stream);
}

static void on_wt_session(cwist_http_request *req,
                          cwist_http_response *res,
                          void *session) {
    const char *path = req && req->path ? req->path->data : "";
    if (strcmp(path, "/wt") != 0) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_webtransport_close_session(session, 404, "unknown WebTransport path");
        return;
    }

    res->status_code = CWIST_HTTP_OK;
    printf("[wt] session accepted on %s\n", path);

    const char hello[] = "hello from cwist webtransport server";
    cwist_webtransport_send_datagram(session, hello, sizeof(hello) - 1);
}

static int bind_udp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    uint16_t port = argc > 1 ? (uint16_t)atoi(argv[1]) : 9443;
    const char *cert = argc > 2 ? argv[2] : "example/othello-web/server.crt";
    const char *key = argc > 3 ? argv[3] : "example/othello-web/server.key";

    int udp_fd = bind_udp(port);
    if (udp_fd < 0) {
        perror("bind udp");
        return 1;
    }

    cwist_http3_context *ctx = NULL;
    cwist_error_t err = cwist_http3_init_context(&ctx, cert, key);
    if (err.error.err_i16 != 0 || !ctx) {
        fprintf(stderr, "failed to initialize HTTP/3 context with %s and %s\n", cert, key);
        close(udp_fd);
        return 1;
    }

    g_ctx = ctx;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    cwist_http3_set_datagram_enabled(ctx, 1);
    cwist_http3_set_datagram_callback(ctx, on_datagram, NULL);
    cwist_http3_set_webtransport_handler(ctx, on_wt_session);
    cwist_webtransport_set_new_stream_handler(ctx, on_wt_stream, NULL);

    printf("WebTransport server: https://localhost:%u/wt\n", port);
    printf("Open ../client/index.html in a WebTransport-capable browser.\n");

    err = cwist_http3_server_loop(udp_fd, ctx, http_fallback, NULL);
    if (err.error.err_i16 != 0) {
        fprintf(stderr, "HTTP/3 server loop failed\n");
    }

    cwist_http3_destroy_context(ctx);
    close(udp_fd);
    return err.error.err_i16 == 0 ? 0 : 1;
}
