#define _GNU_SOURCE
#include <cwist/security/tls/ech.h>
#include <cwist/sys/err/cwist_err.h>
#include <openssl/ssl.h>
#include <openssl/opensslv.h>
#include <stdio.h>

cwist_error_t cwist_app_use_ech(cwist_app *app, const char *ech_key, const char *ech_dir) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!app) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }

#if defined(SSL_OP_ENABLE_ECH) && OPENSSL_VERSION_NUMBER >= 0x30200000L
    if (app->ssl_ctx && app->ssl_ctx->ctx) {
        SSL_CTX_set_options(app->ssl_ctx->ctx, SSL_OP_ENABLE_ECH);
    }
    (void)ech_key;
    (void)ech_dir;
#else
    (void)app;
    (void)ech_key;
    (void)ech_dir;
#endif

    err.error.err_i16 = 0;
    return err;
}
