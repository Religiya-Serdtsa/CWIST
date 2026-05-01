/**
 * @file http3.h
 * @brief HTTP/3 (QUIC-based) Protocol Definitions for CWIST.
 */

#ifndef __CWIST_HTTP3_H__
#define __CWIST_HTTP3_H__

#include <cwist/net/http/http.h>
#include <cwist/sys/err/cwist_err.h>
#include <openssl/ssl.h>
#include <openssl/quic.h>

/** --- HTTP/3 Structures --- */

/**
 * @brief HTTP/3 server context encapsulating OpenSSL QUIC settings.
 */
typedef struct cwist_http3_context {
    SSL_CTX *ssl_ctx; /**< OpenSSL QUIC server context */
    int udp_fd;       /**< Underlying UDP socket descriptor */
} cwist_http3_context;

/**
 * @brief Represents an active HTTP/3 connection via QUIC.
 */
typedef struct cwist_http3_connection {
    SSL *quic_ssl;                       /**< OpenSSL QUIC connection object */
    int udp_fd;                          /**< Shared UDP socket descriptor */
    struct sockaddr_storage peer_addr;   /**< Client's IP address and port */
    socklen_t peer_addr_len;             /**< Length of the peer address structure */
} cwist_http3_connection;

/**
 * @brief Callback function type for handling HTTP/3 requests.
 *
 * @param user_ctx Opaque pointer for user context.
 * @param req Parsed HTTP request object.
 * @param res HTTP response object to be populated by the handler.
 */
typedef void (*cwist_http3_request_handler_func)(void *user_ctx,
                                                 cwist_http_request *req,
                                                 cwist_http_response *res);

/** --- API Functions --- */

/**
 * @brief Initialize an HTTP/3 context for a UDP socket.
 *
 * Creates an OpenSSL QUIC server context, configures ALPN for "h3",
 * and loads the provided TLS certificates.
 *
 * @param ctx Pointer to the newly allocated HTTP/3 context pointer.
 * @param cert_path Path to the PEM-formatted certificate file.
 * @param key_path Path to the PEM-formatted private key file.
 * @return cwist_error_t Error status (0 on success).
 */
cwist_error_t cwist_http3_init_context(cwist_http3_context **ctx,
                                       const char *cert_path,
                                       const char *key_path);

/**
 * @brief Initialize an HTTP/3 context without manual certificates.
 *
 * Generates an ephemeral self-signed RSA certificate in memory.
 * Ideal for local testing or zero-config QUIC setups.
 *
 * @param ctx Pointer to the newly allocated HTTP/3 context pointer.
 * @return cwist_error_t Error status (0 on success).
 */
cwist_error_t cwist_http3_init_context_ephemeral(cwist_http3_context **ctx);

/**
 * @brief Destroy an HTTP/3 context and release its resources.
 *
 * @param ctx The HTTP/3 context to destroy.
 */
void cwist_http3_destroy_context(cwist_http3_context *ctx);

/**
 * @brief Handles an HTTP/3 connection over QUIC.
 *
 * Reads incoming QUIC streams, parses HTTP/3 frames (QPACK),
 * triggers the appropriate user-defined request handler, and multiplexes
 * the response back to the client over QUIC streams.
 *
 * @param conn Pointer to the QUIC/HTTP3 connection structure.
 * @param user_ctx Opaque user context to pass to the handler.
 * @param handler Function pointer to the user's HTTP request handler.
 * @return cwist_error_t Indicates success or connection error.
 */
cwist_error_t cwist_http3_serve_connection(cwist_http3_connection *conn,
                                           void *user_ctx,
                                           cwist_http3_request_handler_func handler);

/**
 * @brief Starts a simple UDP-based event loop for handling HTTP/3 connections.
 *
 * Listens on the provided UDP socket, manages QUIC connections via OpenSSL,
 * and dispatches parsed HTTP/3 requests to the handler.
 *
 * @param udp_fd The bound UDP socket descriptor.
 * @param ctx The initialized HTTP/3 server context.
 * @param handler The application-level request handler.
 * @param user_ctx Opaque pointer passed to the handler for state tracking.
 * @return cwist_error_t Indicates success or server loop failure.
 */
cwist_error_t cwist_http3_server_loop(int udp_fd,
                                      cwist_http3_context *ctx,
                                      cwist_http3_request_handler_func handler,
                                      void *user_ctx);

#endif
