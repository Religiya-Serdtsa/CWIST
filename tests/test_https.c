#include <cwist/sys/app/app.h>
#include <cwist/net/http/https.h>
#include <assert.h>
#include <stdio.h>

static const char *TEST_CERT = "example/othello-web/server.crt";
static const char *TEST_KEY = "example/othello-web/server.key";

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

static void test_app_http2_toggle_rebuilds_context(void) {
    printf("Testing HTTPS/2 toggle context rebuild...\n");
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    assert(app->use_http2 == false);

    cwist_error_t err = cwist_app_use_https2(app, true);
    assert(err.errtype == CWIST_ERR_INT16);
    assert(err.error.err_i16 == 0);
    assert(app->use_http2 == true);
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
    test_https_defaults();
    test_app_http2_toggle_rebuilds_context();
    printf("All HTTPS tests passed!\n");
    return 0;
}
