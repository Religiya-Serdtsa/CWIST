#include <cwist/sys/app/app.h>
#include <cwist/net/http/http3.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

#define TEST_CERT "example/othello-web/server.crt"
#define TEST_KEY  "example/othello-web/server.key"

static volatile int g_wt_handler_called = 0;
static volatile int g_new_stream_called = 0;

static void wt_test_handler(cwist_http_request *req,
                            cwist_http_response *res,
                            void *stream) {
    (void)stream;
    g_wt_handler_called = 1;
    /* Accept the session */
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, "");
}

static void wt_new_stream_handler(void *stream, void *user_ctx) {
    (void)stream;
    (void)user_ctx;
    g_new_stream_called = 1;
}

typedef struct {
    int udp_fd;
    cwist_http3_context *ctx;
} server_thread_args_t;

static void dummy_http3_handler(void *user_ctx, cwist_http_request *req,
                                  cwist_http_response *res) {
    (void)user_ctx;
    (void)req;
    (void)res;
}

static void *wt_server_thread(void *arg) {
    server_thread_args_t *args = (server_thread_args_t *)arg;
    cwist_http3_server_loop(args->udp_fd, args->ctx, dummy_http3_handler, NULL);
    return NULL;
}

int main(void) {
    printf("Testing WebTransport infrastructure...\n");

    /* --- Test 1: App-level WebTransport registration --- */
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    cwist_app_use_webtransport(app, wt_test_handler);
    assert(app->wt_handler == wt_test_handler);
    printf("[PASS] App-level WebTransport handler registration.\n");

    /* --- Test 2: HTTP/3 context-level WebTransport registration --- */
    cwist_http3_context *ctx = NULL;
    cwist_error_t err = cwist_http3_init_context(&ctx, TEST_CERT, TEST_KEY);
    assert(err.error.err_i16 == 0);
    assert(ctx != NULL);

    cwist_http3_set_webtransport_handler(ctx, wt_test_handler);
    assert(ctx->wt_handler == wt_test_handler);

    cwist_webtransport_set_new_stream_handler(ctx, wt_new_stream_handler, (void *)0xdeadbeef);
    assert(ctx->wt_new_stream_handler == wt_new_stream_handler);
    assert(ctx->wt_new_stream_ctx == (void *)0xdeadbeef);
    printf("[PASS] HTTP/3 context-level WebTransport handler registration.\n");

    /* --- Test 3: Server loop with WebTransport enabled --- */
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(udp_fd >= 0);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    assert(bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    server_thread_args_t args = { .udp_fd = udp_fd, .ctx = ctx };
    pthread_t tid;
    int rc = pthread_create(&tid, NULL, wt_server_thread, &args);
    assert(rc == 0);

    usleep(100000);
    assert(ctx->running == 1);

    ctx->running = 0;
    rc = pthread_join(tid, NULL);
    assert(rc == 0);
    printf("[PASS] WebTransport server loop starts and stops gracefully.\n");

    cwist_http3_destroy_context(ctx);
    close(udp_fd);

    /* --- Test 4: I/O API null-safety --- */
    assert(cwist_webtransport_read(NULL, NULL, 0) == -1);
    assert(cwist_webtransport_write(NULL, NULL, 0) == -1);
    assert(cwist_webtransport_flush(NULL) == -1);
    assert(cwist_webtransport_close_stream(NULL) == -1);
    assert(cwist_webtransport_open_bidi_stream(NULL) == -1);
    assert(cwist_webtransport_open_uni_stream(NULL) == -1);
    printf("[PASS] WebTransport I/O API null-safety.\n");

    /* --- Test 5: App-level integration with HTTP/3 refresh --- */
    app = cwist_app_create();
    assert(app != NULL);
    cwist_app_use_webtransport(app, wt_test_handler);
    assert(app->wt_handler == wt_test_handler);

    err = cwist_app_use_http3(app, true);
    assert(err.error.err_i16 == 0);
    assert(app->h3_ctx != NULL);
    assert(app->h3_ctx->wt_handler == wt_test_handler);
    printf("[PASS] App-level WebTransport survives HTTP/3 context refresh.\n");

    cwist_app_destroy(app);

    printf("All WebTransport infrastructure tests passed!\n");
    return 0;
}
