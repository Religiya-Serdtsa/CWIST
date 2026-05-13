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
    cwist_webtransport_handler_func wt_handler; /**< WebTransport session handler */
    void (*wt_new_stream_handler)(void *stream, void *user_ctx); /**< Callback for new WT data streams */
    void *wt_new_stream_ctx; /**< User context for wt_new_stream_handler */
    int idle_timeout_ms;       /**< 0 = use lsquic default (30s) */
    int handshake_timeout_ms;  /**< 0 = use lsquic default (10s) */
    int ping_period_ms;        /**< 0 = use lsquic default (server: none) */
    int noprogress_timeout_ms; /**< 0 = use lsquic default (60s server) */
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

/** @name WebTransport I/O */
/** @{ */

/**
 * @brief Read data from a WebTransport stream.
 *
 * Non-blocking.  Returns number of bytes read, 0 if no data is
 * currently available, or -1 on error.
 *
 * @param stream  Opaque lsquic_stream_t pointer.
 * @param buf     Destination buffer.
 * @param len     Buffer capacity in bytes.
 * @return Number of bytes read, 0 if none available, or -1 on error.
 */
ssize_t cwist_webtransport_read(void *stream, void *buf, size_t len);

/**
 * @brief Write data to a WebTransport stream.
 *
 * Returns number of bytes accepted into the send buffer, or -1 on error.
 *
 * @param stream  Opaque lsquic_stream_t pointer.
 * @param data    Payload to write.
 * @param len     Payload length in bytes.
 * @return Number of bytes buffered, or -1 on error.
 */
ssize_t cwist_webtransport_write(void *stream, const void *data, size_t len);

/**
 * @brief Flush any buffered data on a WebTransport stream.
 *
 * @param stream  Opaque lsquic_stream_t pointer.
 * @return 0 on success, -1 on error.
 */
int cwist_webtransport_flush(void *stream);

/**
 * @brief Close a WebTransport stream.
 *
 * @param stream  Opaque lsquic_stream_t pointer.
 * @return 0 on success, -1 on error.
 */
int cwist_webtransport_close_stream(void *stream);

/**
 * @brief Register a callback for newly created WebTransport data streams.
 *
 * Invoked for both server-initiated and client-initiated streams.
 *
 * @param ctx       HTTP/3 context.
 * @param handler   Callback invoked for each new data stream.
 * @param user_ctx  Opaque pointer forwarded to @p handler.
 */
void cwist_webtransport_set_new_stream_handler(cwist_http3_context *ctx,
                                               void (*handler)(void *stream, void *user_ctx),
                                               void *user_ctx);

/**
 * @brief Request a new server-initiated bidirectional WebTransport stream.
 *
 * The actual stream is delivered asynchronously via the new-stream handler.
 *
 * @param conn  Opaque lsquic_conn_t pointer obtained from the session.
 * @return 0 on success, -1 on failure.
 */
int cwist_webtransport_open_bidi_stream(void *conn);

/**
 * @brief Request a new server-initiated unidirectional WebTransport stream.
 *
 * The actual stream is delivered asynchronously via the new-stream handler.
 *
 * @param conn  Opaque lsquic_conn_t pointer obtained from the session.
 * @return 0 on success, -1 on failure.
 */
int cwist_webtransport_open_uni_stream(void *conn);

/** @} */

/** --- Unstable-network resilience knobs --- */

/**
 * @brief Set the idle timeout for QUIC connections.
 *
 * If the connection is idle for longer than this, it is closed.
 * Default is 0 (lsquic default, typically 30s).
 *
 * @param ctx HTTP/3 context.
 * @param ms  Timeout in milliseconds, or 0 for default.
 */
void cwist_http3_set_idle_timeout(cwist_http3_context *ctx, int ms);

/**
 * @brief Set the handshake timeout.
 *
 * Connections that do not complete the handshake within this time are
 * dropped.  Increase this on high-latency mobile networks.
 *
 * @param ctx HTTP/3 context.
 * @param ms  Timeout in milliseconds, or 0 for default.
 */
void cwist_http3_set_handshake_timeout(cwist_http3_context *ctx, int ms);

/**
 * @brief Set the keep-alive PING period.
 *
 * When non-zero, the server sends PING frames at this interval to keep
 * NAT bindings alive and detect dead peers early.  Recommended 10000-15000
 * ms for mobile clients behind aggressive NATs.
 *
 * @param ctx HTTP/3 context.
 * @param ms  Interval in milliseconds, or 0 to disable.
 */
void cwist_http3_set_ping_period(cwist_http3_context *ctx, int ms);

/**
 * @brief Set the no-progress timeout.
 *
 * Connections that make no forward progress (no acks, no data) for this
 * long are terminated.  Default is 0 (lsquic default, typically 60s on
 * server).
 *
 * @param ctx HTTP/3 context.
 * @param ms  Timeout in milliseconds, or 0 for default.
 */
void cwist_http3_set_noprogress_timeout(cwist_http3_context *ctx, int ms);

#endif
