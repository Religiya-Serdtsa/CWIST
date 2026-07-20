#include <cwist/net/http/http3.h>
#include <cwist/sys/err/cwist_err.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

#define TEST_CERT "example/othello-web/server.crt"
#define TEST_KEY  "example/othello-web/server.key"

int cwist_http3_normalize_response_header_name(const char *name,
                                               char *out,
                                               size_t out_len);
int cwist_http3_response_header_value_is_safe(const char *value);

static volatile int g_handler_called = 0;

static void http3_test_handler(void *user_ctx, cwist_http_request *req,
                               cwist_http_response *res) {
    (void)user_ctx;
    (void)req;
    (void)res;
    g_handler_called = 1;
}

typedef struct {
    int udp_fd;
    cwist_http3_context *ctx;
} server_thread_args_t;

static void *http3_server_thread(void *arg) {
    server_thread_args_t *args = (server_thread_args_t *)arg;
    cwist_http3_server_loop(args->udp_fd, args->ctx, http3_test_handler, NULL);
    return NULL;
}

int main(void) {
    printf("Testing HTTP/3 (BoringSSL + lsquic) infrastructure...\n");

    /* --- Test 1: init_context with valid cert/key --- */
    cwist_http3_context *ctx = NULL;
    cwist_error_t err = cwist_http3_init_context(&ctx, TEST_CERT, TEST_KEY);
    assert(err.error.err_i16 == 0);
    assert(ctx != NULL);
    assert(ctx->ssl_ctx != NULL);
    cwist_http3_destroy_context(ctx);
    printf("[PASS] HTTP/3 context initialization with PEM files.\n");

    /* --- Test 2: init_context with missing cert --- */
    ctx = NULL;
    err = cwist_http3_init_context(&ctx, "/nonexistent/cert.pem",
                                   "/nonexistent/key.pem");
    assert(err.error.err_i16 == -1);
    assert(ctx == NULL);
    printf("[PASS] HTTP/3 context fails gracefully with missing PEM files.\n");

    /* --- Test 3: ephemeral context (self-signed cert) --- */
    ctx = NULL;
    err = cwist_http3_init_context_ephemeral(&ctx);
    assert(err.error.err_i16 == 0);
    assert(ctx != NULL);
    assert(ctx->ssl_ctx != NULL);
    cwist_http3_destroy_context(ctx);
    printf("[PASS] HTTP/3 ephemeral context initialization.\n");

    /* --- Test 4: server_loop rejects invalid args --- */
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(udp_fd >= 0);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    assert(bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    ctx = NULL;
    err = cwist_http3_init_context_ephemeral(&ctx);
    assert(err.error.err_i16 == 0);

    /* Invalid: NULL handler should fail */
    err = cwist_http3_server_loop(udp_fd, ctx, NULL, NULL);
    assert(err.error.err_i16 == -1);
    printf("[PASS] HTTP/3 server_loop rejects NULL handler.\n");

    /* --- Test 5: server_loop runs and stops gracefully --- */
    server_thread_args_t args = { .udp_fd = udp_fd, .ctx = ctx };
    pthread_t tid;
    int rc = pthread_create(&tid, NULL, http3_server_thread, &args);
    assert(rc == 0);

    /* Let the server start polling */
    usleep(50000);

    /* Verify the context is marked running */
    assert(ctx->running == 1);

    /* Stop the server */
    ctx->running = 0;

    rc = pthread_join(tid, NULL);
    assert(rc == 0);
    printf("[PASS] HTTP/3 server_loop starts and stops gracefully.\n");

    cwist_http3_destroy_context(ctx);
    close(udp_fd);

    /* --- Test 6: Advanced features (API smoke test) --- */
    ctx = NULL;
    err = cwist_http3_init_context_ephemeral(&ctx);
    assert(err.error.err_i16 == 0);

    /* Enable server push */
    cwist_http3_set_push_enabled(ctx, 1);
    assert(ctx->push_enabled == 1);
    printf("[PASS] HTTP/3 server push enable API.\n");

    /* Connection migration is on by default */
    assert(ctx->allow_migration == 0); /* not explicitly set yet */
    cwist_http3_destroy_context(ctx);
    printf("[PASS] HTTP/3 connection migration defaults.\n");

    /* --- Test 7: response header normalization for browser strictness --- */
    char h3_name[64];
    assert(cwist_http3_normalize_response_header_name("Set-Cookie",
                                                       h3_name,
                                                       sizeof(h3_name)) == 0);
    assert(strcmp(h3_name, "set-cookie") == 0);
    assert(cwist_http3_normalize_response_header_name("Location",
                                                       h3_name,
                                                       sizeof(h3_name)) == 0);
    assert(strcmp(h3_name, "location") == 0);
    assert(cwist_http3_normalize_response_header_name(":bad",
                                                       h3_name,
                                                       sizeof(h3_name)) == -1);
    assert(cwist_http3_normalize_response_header_name("Bad Header",
                                                       h3_name,
                                                       sizeof(h3_name)) == -1);
    assert(cwist_http3_response_header_value_is_safe("sid=gone; Path=/; Max-Age=0"));
    assert(!cwist_http3_response_header_value_is_safe("ok\r\nbad: value"));
    printf("[PASS] HTTP/3 response headers are lowercased and CRLF-safe.\n");

    printf("All HTTP/3 infrastructure tests passed!\n");
    return 0;
}
