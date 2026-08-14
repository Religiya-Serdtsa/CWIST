/**
 * @file https.h
 * @brief HTTPS/TLS wrapper interface.
 */

#ifndef __CWIST_HTTPS_H__
#define __CWIST_HTTPS_H__

#include <cwist/net/http/http.h>
#include <cwist/sys/err/cwist_err.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define HTTPS_THREAD_POOL_SIZE HTTP_THREAD_POOL_SIZE;

/** --- SSL Structures --- */

typedef enum cwist_https_protocol {
    CWIST_HTTPS_PROTOCOL_NONE = 0,
    CWIST_HTTPS_PROTOCOL_HTTP11,
    CWIST_HTTPS_PROTOCOL_HTTP2,
    CWIST_HTTPS_PROTOCOL_HTTP3
} cwist_https_protocol;

typedef struct cwist_https_context {
    SSL_CTX *ctx;
    bool http2_enabled;
    bool http3_enabled;
    void *ticket_key; ///< Shared session-ticket key material (forked workers inherit it)
} cwist_https_context;

typedef struct cwist_https_connection {
    int fd;
    SSL *ssl;
    char *read_buf;
    size_t buf_len;
    bool negotiated_http2;
    cwist_https_protocol negotiated_protocol;
    bool http3_enabled;
    bool http2_sequenced_data; /*< Enable CWIST-specific sequenced DATA frames. */
} cwist_https_connection;

typedef struct cwist_app cwist_app;

typedef struct cwist_https_options {
    bool enable_http2;
    bool enable_http3;
} cwist_https_options;

/** --- API Functions --- */

/**
 * Initialize the OpenSSL library and create an SSL context.
 * Loads certificate and private key.
 */
cwist_error_t cwist_https_init_context(cwist_https_context **ctx, const char *cert_path, const char *key_path);

/**
 * Initialize an HTTPS context with explicit transport options.
 * The HTTP/2 option only applies the standard TLS/ALPN profile today.
 * Application request handling remains HTTP/1.1 unless a frame engine is added.
 */
cwist_error_t cwist_https_init_context_with_options(cwist_https_context **ctx,
                                                    const char *cert_path,
                                                    const char *key_path,
                                                    const cwist_https_options *options,
                                                    cwist_app *app);

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
 * Returns true when ALPN negotiated h2 on this TLS connection.
 */
bool cwist_https_connection_uses_http2(const cwist_https_connection *conn);

/**
 * Returns the protocol selected by ALPN, or HTTP/1.1 when no higher protocol matched.
 */
cwist_https_protocol cwist_https_connection_protocol(const cwist_https_connection *conn);

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
 * Streams the header block and body separately (zero-copy; no intermediate
 * serialization blob) and supports pointer bodies and file streams.
 */
cwist_error_t cwist_https_send_response(cwist_https_connection *conn, cwist_http_response *res);

/**
 * Helper to start a simple HTTPS server loop.
 * Note: The handler receives a cwist_https_connection pointer, not an int fd.
 */
cwist_error_t cwist_https_server_loop(int server_fd, cwist_https_context *ctx, void (*handler)(cwist_https_connection *conn, void *), void *user_ctx);

/**
 * Thread pool helpers for hybrid async-accept + threaded-process mode.
 */
int https_pool_init(void);
void https_pool_submit(int client_fd, cwist_https_context *ctx, void (*handler)(cwist_https_connection *, void *), void *user_ctx);
void https_pool_destroy(void);

/** --- Error Codes --- */
/**
 * @brief Defined as errno-like constants used with `cwist_error_t` fields.
 * Values are declared elsewhere alongside their implementations.
 */

#endif
