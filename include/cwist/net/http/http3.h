/**
 * @file http3.h
 * @brief HTTP/3 (QUIC-based) Protocol Definitions for CWIST.
 */

#ifndef __CWIST_HTTP3_H__
#define __CWIST_HTTP3_H__

#include <cwist/net/http/http.h>
#include <cwist/sys/err/cwist_err.h>
#include <openssl/ssl.h>

/** --- Forward Declarations --- */

typedef struct cwist_http3_context cwist_http3_context;

/**
 * @brief Callback function type for handling WebTransport sessions over HTTP/3.
 *
 * @param req Parsed HTTP request object (CONNECT with :protocol=webtransport).
 * @param res HTTP response object to be populated (e.g., 200 OK to accept).
 * @param stream Opaque lsquic_stream_t pointer for the WebTransport session.
 */
typedef void (*cwist_webtransport_handler_func)(cwist_http_request *req,
                                                 cwist_http_response *res,
                                                 void *stream);

/** --- HTTP/3 Structures --- */

/**
 * @brief HTTP/3 server context encapsulating lsquic/BoringSSL settings.
 */
struct cwist_http3_context {
    SSL_CTX *ssl_ctx; /**< BoringSSL server context */
    int udp_fd;       /**< Underlying UDP socket descriptor */
    void *engine;     /**< Opaque lsquic_engine_t pointer */
    void (*handler)(void *user_ctx, cwist_http_request *req, cwist_http_response *res);
    void *user_ctx;
    volatile int running; /**< Set to 0 to gracefully stop server_loop */
    int push_enabled;     /**< HTTP/3 server push enabled */
    int early_data_enabled; /**< 0-RTT early data enabled */
    int allow_migration;  /**< QUIC connection migration enabled */
    int datagram_enabled; /**< QUIC datagram extension enabled */
    void (*datagram_cb)(const void *data, size_t len, void *user_ctx);
    void *datagram_user_ctx;
    cwist_webtransport_handler_func wt_handler;
};

/**
 * @brief Represents an active HTTP/3 connection via QUIC.
 */
typedef struct cwist_http3_connection {
    void *conn;                          /**< Opaque lsquic_conn_t pointer */
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

/**
 * @brief Callback function type for handling WebTransport sessions over HTTP/3.
 *
 * @param req Parsed HTTP request object (CONNECT with :protocol=webtransport).
 * @param res HTTP response object to be populated (e.g., 200 OK to accept).
 * @param stream Opaque lsquic_stream_t pointer for the WebTransport session.
 */
/** --- API Functions --- */

/**
 * @brief Initialize an HTTP/3 context with a certificate.
 */
cwist_error_t cwist_http3_init_context(cwist_http3_context **ctx,
                                       const char *cert_path,
                                       const char *key_path);

/**
 * @brief Initialize an HTTP/3 context with an ephemeral self-signed certificate.
 */
cwist_error_t cwist_http3_init_context_ephemeral(cwist_http3_context **ctx);

/**
 * @brief Destroy an HTTP/3 context.
 */
void cwist_http3_destroy_context(cwist_http3_context *ctx);

/**
 * @brief Serve a single HTTP/3 connection.
 */
cwist_error_t cwist_http3_serve_connection(cwist_http3_connection *conn,
                                           void *user_ctx,
                                           cwist_http3_request_handler_func handler);

/**
 * @brief Run the HTTP/3 server event loop.
 */
cwist_error_t cwist_http3_server_loop(int udp_fd,
                                      cwist_http3_context *ctx,
                                      cwist_http3_request_handler_func handler,
                                      void *user_ctx);

/** --- Advanced Features --- */

/**
 * @brief Enable or disable HTTP/3 server push on the given context.
 *
 * Push is disabled by default.  When enabled, the handler can push
 * resources by calling cwist_http3_push_resource().
 */
void cwist_http3_set_push_enabled(cwist_http3_context *ctx, int enabled);

/**
 * @brief Push a resource to the client.
 *
 * This should be called from inside the request handler callback.
 * The push is associated with the current request stream.
 *
 * @param req           The current request (used to find the underlying stream).
 * @param path          Resource path to push (e.g. "/style.css").
 * @param content_type  Optional Content-Type header value (may be NULL).
 * @return 0 on success, -1 on failure.
 */
int cwist_http3_push_resource(cwist_http_request *req,
                              const char *path,
                              const char *content_type);

/**
 * @brief Set stream priority for the current response.
 *
 * Valid priority values are 1 through 256, inclusive.
 * Lower value means higher priority.
 */
int cwist_http3_set_stream_priority(cwist_http_request *req, unsigned priority);

/** --- Datagram API --- */

/**
 * @brief Enable or disable QUIC datagram extension on the given context.
 *
 * Datagram is disabled by default.  When enabled, the application can
 * send and receive unreliable datagrams over QUIC.
 */
void cwist_http3_set_datagram_enabled(cwist_http3_context *ctx, int enabled);

/**
 * @brief Register a callback for incoming QUIC datagrams.
 *
 * @param ctx       HTTP/3 context.
 * @param cb        Callback invoked for each received datagram.
 * @param user_ctx  Opaque pointer forwarded to @p cb.
 */
void cwist_http3_set_datagram_callback(cwist_http3_context *ctx,
                                       void (*cb)(const void *data, size_t len, void *user_ctx),
                                       void *user_ctx);

/**
 * @brief Send an unreliable datagram over the given QUIC connection.
 *
 * This is non-blocking; the datagram is queued and flushed by the engine.
 *
 * @param conn  Target QUIC connection.
 * @param data  Datagram payload.
 * @param len   Payload length in bytes.
 * @return 0 on success, -1 on failure.
 */
int cwist_http3_send_datagram(void *conn, const void *data, size_t len);

/**
 * @brief Register a WebTransport session handler on the given HTTP/3 context.
 *
 * When an HTTP/3 CONNECT request with `:protocol=webtransport` is received,
 * the regular request handler is bypassed and @p handler is invoked instead.
 *
 * @param ctx       HTTP/3 context.
 * @param handler   WebTransport session handler.
 */
void cwist_http3_set_webtransport_handler(cwist_http3_context *ctx,
                                          cwist_webtransport_handler_func handler);

#endif
