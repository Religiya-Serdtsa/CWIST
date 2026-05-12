#include <cwist/sys/app/app.h>
#include <cwist/net/http/https.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>

static const char *TEST_CERT = "example/othello-web/server.crt";
static const char *TEST_KEY = "example/othello-web/server.key";

typedef struct alpn_server_ctx {
    int fd;
    cwist_https_context *ctx;
    cwist_https_protocol protocol;
    cwist_error_t result;
} alpn_server_ctx;

static void *alpn_server_thread(void *arg) {
    alpn_server_ctx *server = arg;
    cwist_https_connection *conn = NULL;
    server->result = cwist_https_accept(server->ctx, server->fd, &conn);
    if (server->result.errtype == CWIST_ERR_INT16 && server->result.error.err_i16 == 0) {
        server->protocol = cwist_https_connection_protocol(conn);
        cwist_https_close_connection(conn);
    } else {
        close(server->fd);
    }
    return NULL;
}

static void run_alpn_client(int fd, const unsigned char *protos, unsigned int protos_len) {
    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, fd) == 1);
    if (protos && protos_len > 0) {
        assert(SSL_set_alpn_protos(client, protos, protos_len) == 0);
    }
    assert(SSL_connect(client) == 1);
    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(fd);
}

static void test_https_defaults(void) {
    printf("Testing HTTPS defaults...\n");
    cwist_https_context *ctx = NULL;
    cwist_error_t err = cwist_https_init_context(&ctx, TEST_CERT, TEST_KEY);
    assert(err.errtype == CWIST_ERR_INT16);
    assert(err.error.err_i16 == 0);
    assert(ctx != NULL);
    assert(ctx->http2_enabled == false);
    assert(SSL_CTX_get_min_proto_version(ctx->ctx) == TLS1_2_VERSION);
    cwist_https_destroy_context(ctx);
    printf("Passed HTTPS defaults.\n");
}

static void test_https_alpn_negotiates_h2_and_http11_fallback(void) {
    printf("Testing HTTPS ALPN negotiation and HTTP/1.1 fallback...\n");
    cwist_https_options options = { .enable_http2 = true };
    cwist_https_context *ctx = NULL;
    cwist_error_t err = cwist_https_init_context_with_options(&ctx, TEST_CERT, TEST_KEY, &options);
    assert(err.errtype == CWIST_ERR_INT16);
    assert(err.error.err_i16 == 0);

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    alpn_server_ctx h2_server = { .fd = sv[0], .ctx = ctx, .protocol = CWIST_HTTPS_PROTOCOL_NONE };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, alpn_server_thread, &h2_server) == 0);
    static const unsigned char h2_client_alpn[] = "\x02h2\x08http/1.1";
    run_alpn_client(sv[1], h2_client_alpn, sizeof(h2_client_alpn) - 1);
    pthread_join(tid, NULL);
    assert(h2_server.protocol == CWIST_HTTPS_PROTOCOL_HTTP2);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    alpn_server_ctx http11_server = { .fd = sv[0], .ctx = ctx, .protocol = CWIST_HTTPS_PROTOCOL_NONE };
    assert(pthread_create(&tid, NULL, alpn_server_thread, &http11_server) == 0);
    static const unsigned char http11_client_alpn[] = "\x08http/1.1";
    run_alpn_client(sv[1], http11_client_alpn, sizeof(http11_client_alpn) - 1);
    pthread_join(tid, NULL);
    assert(http11_server.protocol == CWIST_HTTPS_PROTOCOL_HTTP11);

    cwist_https_destroy_context(ctx);
    printf("Passed HTTPS ALPN negotiation and HTTP/1.1 fallback.\n");
}

static void test_app_http2_toggle_rebuilds_context(void) {
    printf("Testing HTTPS/2 toggle context rebuild...\n");
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    assert(app->use_https2 == false);

    cwist_error_t err = cwist_app_use_https2(app, true);
    assert(err.errtype == CWIST_ERR_INT16);
    assert(err.error.err_i16 == 0);
    assert(app->use_https2 == true);
    assert(app->ssl_ctx == NULL);

    err = cwist_app_use_https(app, TEST_CERT, TEST_KEY);
    assert(err.errtype == CWIST_ERR_INT16);
    assert(err.error.err_i16 == 0);
    assert(app->ssl_ctx != NULL);
    assert(app->ssl_ctx->http2_enabled == true);

    err = cwist_app_use_https2(app, false);
    assert(err.errtype == CWIST_ERR_INT16);
    assert(err.error.err_i16 == 0);
    assert(app->ssl_ctx != NULL);
    assert(app->ssl_ctx->http2_enabled == false);

    cwist_app_destroy(app);
    printf("Passed HTTPS/2 toggle context rebuild.\n");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_https_defaults();
    test_https_alpn_negotiates_h2_and_http11_fallback();
    test_app_http2_toggle_rebuilds_context();
    printf("All HTTPS tests passed!\n");
    return 0;
}
