#ifndef __CWIST_HTTPS_H__
#define __CWIST_HTTPS_H__

#include <cwist/net/http/http.h>
#include <cwist/sys/err/cwist_err.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

/* --- SSL Structures --- */

typedef struct cwist_https_context {
    SSL_CTX *ctx;
} cwist_https_context;

typedef struct cwist_https_connection {
    int fd;
    SSL *ssl;
    char *read_buf;
    size_t buf_len;
} cwist_https_connection;

/* --- API Functions --- */

/**
 * Initialize the OpenSSL library and create an SSL context.
 * Loads certificate and private key.
 */
cwist_error_t cwist_https_init_context(cwist_https_context **ctx, const char *cert_path, const char *key_path);

/**
 * Destroy the HTTPS context and cleanup OpenSSL.
 */
void cwist_https_destroy_context(cwist_https_context *ctx);

/**
 * Perform SSL handshake on an accepted socket.
 * Returns a new cwist_https_connection wrapper.
 */
cwist_error_t cwist_https_accept(cwist_https_context *ctx, int client_fd, cwist_https_connection **conn);

/**
 * Close and free the HTTPS connection.
 */
void cwist_https_close_connection(cwist_https_connection *conn);

/**
 * Read data from the SSL connection and parse it as an HTTP request.
 * Uses cwist_http_parse_request internally.
 */
cwist_http_request *cwist_https_receive_request(cwist_https_connection *conn);

/**
 * Serialize an HTTP response and send it over the SSL connection.
 * Uses cwist_http_stringify_response internally.
 */
cwist_error_t cwist_https_send_response(cwist_https_connection *conn, cwist_http_response *res);

/**
 * Helper to start a simple HTTPS server loop.
 * Note: The handler receives a cwist_https_connection pointer, not an int fd.
 */
cwist_error_t cwist_https_server_loop(int server_fd, cwist_https_context *ctx, void (*handler)(cwist_https_connection *conn, void *), void *user_ctx);

/* --- Error Codes --- */
// Defined as constants to be used with cwist_error_t (err_i16) or err_json
// We will use standard errno-like negative values or specific error codes.

#endif
