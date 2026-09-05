#include <cwist/net/http/http3.h>
#include <cwist/net/http/http3_client.h>
#include <cwist/sys/err/cwist_err.h>
#include <lsquic.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <fcntl.h>

#define TEST_CERT "example/othello-web/server.crt"
#define TEST_KEY  "example/othello-web/server.key"

int cwist_http3_normalize_response_header_name(const char *name,
                                               char *out,
                                               size_t out_len);
int cwist_http3_response_header_value_is_safe(const char *value);
int cwist_http3_method_is_idempotent(const char *method_str);

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

    /* The server loop must force the UDP socket into non-blocking mode */
    int fl = fcntl(udp_fd, F_GETFL, 0);
    assert(fl >= 0 && (fl & O_NONBLOCK));
    printf("[PASS] HTTP/3 server_loop sets O_NONBLOCK on the UDP socket.\n");

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

    /* --- Test 8: 0-RTT early data setter (opt-in, default OFF) --- */
    ctx = NULL;
    err = cwist_http3_init_context_ephemeral(&ctx);
    assert(err.error.err_i16 == 0);
    assert(ctx->early_data_enabled == 0);
    assert(ctx->early_data_guard == 0);

    cwist_http3_set_early_data(ctx, true);
    assert(ctx->early_data_enabled == 1);
    /* Guard defaults ON when early data is enabled */
    assert(ctx->early_data_guard == 1);

    cwist_http3_set_early_data_guard(ctx, 0);
    assert(ctx->early_data_guard == 0);
    cwist_http3_set_early_data_guard(ctx, 1);
    assert(ctx->early_data_guard == 1);

    cwist_http3_set_early_data(ctx, false);
    assert(ctx->early_data_enabled == 0);
    cwist_http3_destroy_context(ctx);
    printf("[PASS] HTTP/3 0-RTT early data setter and replay guard defaults.\n");

    /* Same behavior on the certificate-based init path */
    ctx = NULL;
    err = cwist_http3_init_context(&ctx, TEST_CERT, TEST_KEY);
    assert(err.error.err_i16 == 0);
    assert(ctx->early_data_enabled == 0);
    cwist_http3_set_early_data(ctx, true);
    assert(ctx->early_data_enabled == 1 && ctx->early_data_guard == 1);
    cwist_http3_destroy_context(ctx);
    printf("[PASS] HTTP/3 0-RTT setter works on certificate-based context.\n");

    /* --- Test 9: replay guard method classification --- */
    assert(cwist_http3_method_is_idempotent("GET") == 1);
    assert(cwist_http3_method_is_idempotent("HEAD") == 1);
    assert(cwist_http3_method_is_idempotent("OPTIONS") == 1);
    assert(cwist_http3_method_is_idempotent("PUT") == 1);
    assert(cwist_http3_method_is_idempotent("DELETE") == 1);
    assert(cwist_http3_method_is_idempotent("TRACE") == 1);
    assert(cwist_http3_method_is_idempotent("POST") == 0);
    assert(cwist_http3_method_is_idempotent("PATCH") == 0);
    assert(cwist_http3_method_is_idempotent("CONNECT") == 0);
    assert(cwist_http3_method_is_idempotent(NULL) == 0);
    assert(cwist_http3_method_is_idempotent("garbage") == 0);
    printf("[PASS] HTTP/3 replay guard method classification.\n");

    /* --- Test 10: RFC 9114 Section 4.3.1 pseudo-header validation over the
     * wire: a well-formed request is dispatched, an unknown pseudo-header is
     * rejected with 400 and never reaches the handler. --- */
    ctx = NULL;
    err = cwist_http3_init_context_ephemeral(&ctx);
    assert(err.error.err_i16 == 0);

    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(udp_fd >= 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    assert(bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t addr_len = sizeof(addr);
    assert(getsockname(udp_fd, (struct sockaddr *)&addr, &addr_len) == 0);

    server_thread_args_t args10 = { .udp_fd = udp_fd, .ctx = ctx };
    rc = pthread_create(&tid, NULL, http3_server_thread, &args10);
    assert(rc == 0);
    usleep(50000);

    cwist_http3_client *client = cwist_http3_client_create();
    assert(client != NULL);
    assert(cwist_http3_client_set_server(client, "127.0.0.1",
                                         ntohs(addr.sin_port)) == 0);
    cwist_http3_client_set_timeout_ms(client, 10000);
    cwist_http3_client_set_conn_timeout_ms(client, 10000);

    /* Verification is on by default: the ephemeral self-signed certificate
     * must fail the handshake, and the failure must surface as a request
     * error, not a crash or a fake response. */
    g_handler_called = 0;
    cwist_http_response *res = NULL;
    err = cwist_http3_client_request(client, "/", CWIST_HTTP_GET,
                                     NULL, NULL, 0, &res);
    assert(err.error.err_i16 != 0);
    assert(res == NULL);
    assert(g_handler_called == 0);
    printf("[PASS] HTTP/3 self-signed server rejected by default verify.\n");

    /* Explicit opt-out for the self-signed development certificate. */
    cwist_http3_client_set_insecure(client, 1);

    /* Well-formed request: handler runs, 200 comes back. */
    g_handler_called = 0;
    res = NULL;
    err = cwist_http3_client_request(client, "/", CWIST_HTTP_GET,
                                     NULL, NULL, 0, &res);
    assert(err.error.err_i16 == 0);
    assert(res != NULL);
    assert(g_handler_called == 1);
    assert(res->status_code == 200);
    cwist_http_response_destroy(res);
    printf("[PASS] HTTP/3 well-formed request is dispatched (200).\n");

    /* Unknown pseudo-header: malformed, 400, handler must not run. */
    g_handler_called = 0;
    cwist_http_header_node *hdrs = NULL;
    cwist_http_header_add(&hdrs, ":bogus", "1");
    res = NULL;
    err = cwist_http3_client_request(client, "/", CWIST_HTTP_GET,
                                     hdrs, NULL, 0, &res);
    cwist_http_header_free_all(hdrs);
    assert(err.error.err_i16 == 0);
    assert(res != NULL);
    assert(res->status_code == CWIST_HTTP_BAD_REQUEST);
    assert(g_handler_called == 0);
    cwist_http_response_destroy(res);
    printf("[PASS] HTTP/3 unknown pseudo-header rejected with 400.\n");

    /* Plain CONNECT carrying :scheme/:path is malformed (RFC 9114 4.3.1):
     * the client always sends :scheme and :path, so this must be rejected. */
    g_handler_called = 0;
    res = NULL;
    err = cwist_http3_client_request(client, "/", CWIST_HTTP_CONNECT,
                                     NULL, NULL, 0, &res);
    assert(err.error.err_i16 == 0);
    assert(res != NULL);
    assert(res->status_code == CWIST_HTTP_BAD_REQUEST);
    assert(g_handler_called == 0);
    cwist_http_response_destroy(res);
    printf("[PASS] HTTP/3 CONNECT with :scheme/:path rejected with 400.\n");

    cwist_http3_client_destroy(client);
    ctx->running = 0;
    rc = pthread_join(tid, NULL);
    assert(rc == 0);
    cwist_http3_destroy_context(ctx);
    close(udp_fd);

    /* --- Test 11: certificate verification succeeds when the self-signed
     * server certificate (CA:TRUE) is loaded as the trust anchor. --- */
    ctx = NULL;
    err = cwist_http3_init_context(&ctx, TEST_CERT, TEST_KEY);
    assert(err.error.err_i16 == 0);

    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(udp_fd >= 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    assert(bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    addr_len = sizeof(addr);
    assert(getsockname(udp_fd, (struct sockaddr *)&addr, &addr_len) == 0);

    server_thread_args_t args11 = { .udp_fd = udp_fd, .ctx = ctx };
    rc = pthread_create(&tid, NULL, http3_server_thread, &args11);
    assert(rc == 0);
    usleep(50000);

    client = cwist_http3_client_create();
    assert(client != NULL);
    /* TEST_CERT is issued for CN=localhost */
    assert(cwist_http3_client_set_server(client, "localhost",
                                         ntohs(addr.sin_port)) == 0);
    assert(cwist_http3_client_set_ca_bundle(client, TEST_CERT) == 0);
    cwist_http3_client_set_timeout_ms(client, 10000);
    cwist_http3_client_set_conn_timeout_ms(client, 10000);

    g_handler_called = 0;
    res = NULL;
    err = cwist_http3_client_request(client, "/", CWIST_HTTP_GET,
                                     NULL, NULL, 0, &res);
    assert(err.error.err_i16 == 0);
    assert(res != NULL);
    assert(res->status_code == 200);
    assert(g_handler_called == 1);
    cwist_http_response_destroy(res);
    printf("[PASS] HTTP/3 verify succeeds with CA bundle for server cert.\n");

    cwist_http3_client_destroy(client);
    ctx->running = 0;
    rc = pthread_join(tid, NULL);
    assert(rc == 0);
    cwist_http3_destroy_context(ctx);
    close(udp_fd);

    printf("All HTTP/3 infrastructure tests passed!\n");
    lsquic_global_cleanup();
    return 0;
}
