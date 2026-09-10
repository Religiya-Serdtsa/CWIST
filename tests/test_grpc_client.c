#define _POSIX_C_SOURCE 200809L
/* gRPC client tests: real h2c calls over loopback TCP against the in-tree
 * gRPC server (unary echo, server-streaming, error status, deadline). */
#include <cwist/sys/app/app.h>
#include <cwist/net/grpc/grpc.h>
#include <cwist/net/grpc/grpc_client.h>
#include <cwist/net/http/http2.h>
#include <cwist/net/http/https.h>
#include <cwist/core/mem/alloc.h>
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* --- test server: loopback listener, one thread per h2c connection --- */

static cwist_app *g_app;
static int g_listen_fd = -1;
static pthread_t g_accept_tid;

#define MAX_CONN_THREADS 16
static pthread_t g_conn_tids[MAX_CONN_THREADS];
static int g_conn_count;
static pthread_mutex_t g_conn_mu = PTHREAD_MUTEX_INITIALIZER;

static void cli_echo(cwist_http_request *req, cwist_http_response *res,
                     const cwist_grpc_message *message, void *user_ctx) {
    (void)req;
    (void)user_ctx;
    cwist_grpc_set_response(res, CWIST_GRPC_OK, NULL, message->data, message->len);
}

static void cli_fail(cwist_http_request *req, cwist_http_response *res,
                     const cwist_grpc_message *message, void *user_ctx) {
    (void)req;
    (void)message;
    (void)user_ctx;
    cwist_grpc_set_error(res, CWIST_GRPC_INVALID_ARGUMENT, "bad input");
}

static void cli_lots(cwist_grpc_stream *stream, void *user_ctx) {
    (void)user_ctx;
    cwist_grpc_message msg;
    while (cwist_grpc_stream_recv(stream, &msg) == 1) {
        for (int i = 0; i < 3; i++) {
            char text[32];
            snprintf(text, sizeof(text), "chunk-%d", i + 1);
            cwist_pb_writer w;
            cwist_pb_writer_init(&w);
            assert(cwist_pb_write_string_field(&w, 1, text) == 0);
            assert(cwist_grpc_stream_send(stream, w.data, w.len) == 0);
            cwist_pb_writer_free(&w);
        }
    }
    cwist_grpc_stream_close(stream, CWIST_GRPC_OK, NULL);
}

static void cli_block(cwist_grpc_stream *stream, void *user_ctx) {
    (void)user_ctx;
    cwist_grpc_message msg;
    while (cwist_grpc_stream_recv(stream, &msg) == 1)
        ;
    /* Never answer on its own; only the deadline/cancel ends the call.
     * Bounded so the handler thread always winds down. */
    for (int i = 0; i < 1000 && !cwist_grpc_stream_cancelled(stream); i++) {
        struct timespec ts = { 0, 10000000L };
        nanosleep(&ts, NULL);
    }
    cwist_grpc_stream_close(stream, stream->status, stream->status_message);
}

static void cli_bridge(void *user_ctx, cwist_http_request *req,
                       cwist_http_response *res) {
    cwist_app *app = user_ctx;
    req->app = app;
    cwist_app_dispatch(app, req, res);
}

static void *serve_one(void *arg) {
    int fd = *(int *)arg;
    cwist_free(arg);
    cwist_https_connection conn = {
        .fd = fd,
        .ssl = NULL,
        .read_buf = NULL,
        .buf_len = 0,
        .negotiated_http2 = true,
        .negotiated_protocol = CWIST_HTTPS_PROTOCOL_HTTP2,
        .http2_sequenced_data = false
    };
    cwist_http2_serve_connection_ex(&conn, g_app, cli_bridge,
                                    cwist_grpc_http2_hooks());
    close(fd);
    return NULL;
}

static void *accept_loop(void *arg) {
    (void)arg;
    for (;;) {
        int cfd = accept(g_listen_fd, NULL, NULL);
        if (cfd < 0) break;
        int *owned = cwist_alloc(sizeof(*owned));
        assert(owned != NULL);
        *owned = cfd;
        pthread_t tid;
        assert(pthread_create(&tid, NULL, serve_one, owned) == 0);
        pthread_mutex_lock(&g_conn_mu);
        assert(g_conn_count < MAX_CONN_THREADS);
        g_conn_tids[g_conn_count++] = tid;
        pthread_mutex_unlock(&g_conn_mu);
    }
    return NULL;
}

static uint16_t start_server(void) {
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(g_listen_fd >= 0);
    int one = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(g_listen_fd, 16) == 0);
    socklen_t alen = sizeof(addr);
    assert(getsockname(g_listen_fd, (struct sockaddr *)&addr, &alen) == 0);
    assert(pthread_create(&g_accept_tid, NULL, accept_loop, NULL) == 0);
    return ntohs(addr.sin_port);
}

static void stop_server(void) {
    shutdown(g_listen_fd, SHUT_RDWR);
    close(g_listen_fd);
    pthread_join(g_accept_tid, NULL);
    pthread_mutex_lock(&g_conn_mu);
    for (int i = 0; i < g_conn_count; i++) pthread_join(g_conn_tids[i], NULL);
    g_conn_count = 0;
    pthread_mutex_unlock(&g_conn_mu);
}

/* --- client-side helpers --- */

static uint8_t *build_echo_request(const char *text, size_t *out_len) {
    cwist_pb_writer w;
    cwist_pb_writer_init(&w);
    assert(cwist_pb_write_string_field(&w, 1, text) == 0);
    uint8_t *copy = cwist_alloc(w.len);
    assert(copy != NULL);
    memcpy(copy, w.data, w.len);
    *out_len = w.len;
    cwist_pb_writer_free(&w);
    return copy;
}

static void assert_echo_reply(const cwist_grpc_message *msg, const char *expected) {
    cwist_pb_reader r;
    cwist_pb_reader_init(&r, msg->data, msg->len);
    cwist_pb_field field;
    assert(cwist_pb_read_field(&r, &field) > 0);
    assert(field.number == 1 && field.wire_type == CWIST_PB_LEN);
    assert(field.len == strlen(expected));
    assert(memcmp(field.bytes, expected, field.len) == 0);
}

static void test_unary(uint16_t port) {
    printf("  unary round trip...\n");
    cwist_grpc_client *client = cwist_grpc_client_connect("127.0.0.1", port, NULL);
    assert(client != NULL);

    size_t req_len;
    uint8_t *req = build_echo_request("ping", &req_len);
    cwist_grpc_call *call = cwist_grpc_call_start(client, "/cwist.test.Cli/Echo",
                                                  req, req_len, 0);
    assert(call != NULL);

    cwist_grpc_message msg;
    assert(cwist_grpc_call_recv(call, &msg) == 1);
    assert_echo_reply(&msg, "ping");
    assert(cwist_grpc_call_recv(call, &msg) == 0);

    const char *status_message = NULL;
    assert(cwist_grpc_call_finish(call, &status_message) == CWIST_GRPC_OK);

    cwist_grpc_call_destroy(call);
    cwist_free(req);
    cwist_grpc_client_close(client);
}

static void test_server_streaming(uint16_t port) {
    printf("  server streaming...\n");
    cwist_grpc_client *client = cwist_grpc_client_connect("127.0.0.1", port, NULL);
    assert(client != NULL);

    cwist_grpc_call *call = cwist_grpc_call_start(client, "/cwist.test.Cli/Lots",
                                                  NULL, 0, 0);
    assert(call != NULL);

    cwist_grpc_message msg;
    int count = 0;
    int rc;
    while ((rc = cwist_grpc_call_recv(call, &msg)) == 1) {
        char expected[32];
        snprintf(expected, sizeof(expected), "chunk-%d", count + 1);
        assert_echo_reply(&msg, expected);
        count++;
    }
    assert(rc == 0);
    assert(count == 3);

    const char *status_message = NULL;
    assert(cwist_grpc_call_finish(call, &status_message) == CWIST_GRPC_OK);

    cwist_grpc_call_destroy(call);
    cwist_grpc_client_close(client);
}

static void test_error_status(uint16_t port) {
    printf("  error status surfacing...\n");
    cwist_grpc_client *client = cwist_grpc_client_connect("127.0.0.1", port, NULL);
    assert(client != NULL);

    cwist_grpc_call *call = cwist_grpc_call_start(client, "/cwist.test.Cli/Fail",
                                                  NULL, 0, 0);
    assert(call != NULL);

    cwist_grpc_message msg;
    int rc;
    while ((rc = cwist_grpc_call_recv(call, &msg)) == 1)
        ; /* error responses are Trailers-Only: no message, status in trailers */
    assert(rc == 0);
    const char *status_message = NULL;
    cwist_grpc_status_t status = cwist_grpc_call_finish(call, &status_message);
    assert(status == CWIST_GRPC_INVALID_ARGUMENT);
    assert(status_message != NULL);
    assert(strstr(status_message, "bad input") != NULL);

    cwist_grpc_call_destroy(call);
    cwist_grpc_client_close(client);
}

static void test_deadline(uint16_t port) {
    printf("  deadline enforcement...\n");
    cwist_grpc_client *client = cwist_grpc_client_connect("127.0.0.1", port, NULL);
    assert(client != NULL);

    /* The server never answers within 150 ms; the deadline is enforced
     * either server-side (trailers) or client-side (local RST_STREAM). */
    cwist_grpc_call *call = cwist_grpc_call_start(client, "/cwist.test.Cli/Block",
                                                  NULL, 0, 150);
    assert(call != NULL);

    cwist_grpc_message msg;
    int rc = cwist_grpc_call_recv(call, &msg);
    assert(rc == 0 || rc == -1); /* no message may arrive */
    const char *status_message = NULL;
    assert(cwist_grpc_call_finish(call, &status_message) == CWIST_GRPC_DEADLINE_EXCEEDED);

    cwist_grpc_call_destroy(call);
    cwist_grpc_client_close(client);

    /* Let the cancelled server-side handler thread wind down so the
     * session (and its request arena) is released before app teardown. */
    struct timespec settle = { 0, 300000000L };
    nanosleep(&settle, NULL);
}

int main(void) {
    printf("Testing gRPC client...\n");

    g_app = cwist_app_create();
    assert(g_app != NULL);
    assert(cwist_app_grpc_unary(g_app, "cwist.test.Cli", "Echo", cli_echo, NULL) == 0);
    assert(cwist_app_grpc_unary(g_app, "cwist.test.Cli", "Fail", cli_fail, NULL) == 0);
    assert(cwist_app_grpc_stream(g_app, "cwist.test.Cli", "Lots", cli_lots, NULL) == 0);
    assert(cwist_app_grpc_stream(g_app, "cwist.test.Cli", "Block", cli_block, NULL) == 0);

    uint16_t port = start_server();

    test_unary(port);
    test_server_streaming(port);
    test_error_status(port);
    test_deadline(port);

    stop_server();
    cwist_app_destroy(g_app);
    printf("test_grpc_client: OK\n");
    return 0;
}
