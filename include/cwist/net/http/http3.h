/**
 * @file http3.h
 * @brief HTTP/3 (QUIC-based) Protocol Definitions for CWIST.
 */

#ifndef __CWIST_HTTP3_H__
#define __CWIST_HTTP3_H__

#include <cwist/net/http/http.h>
#include <cwist/sys/err/cwist_err.h>
#include <openssl/ssl.h>
#include <stdbool.h>
#include <stdint.h>

/** --- Forward Declarations --- */

typedef struct cwist_http3_context cwist_http3_context;

/**
 * @brief Callback function type for handling WebTransport sessions over HTTP/3.
 *
 * @param req Parsed HTTP request object (CONNECT with :protocol=webtransport).
 * @param res HTTP response object to be populated (e.g., 200 OK to accept).
 * @param stream Opaque CWIST WebTransport session handle.
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
    int early_data_guard;   /**< Restrict 0-RTT requests to idempotent methods */
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
    void *hsets; /**< Head of tracked cwist_h3_hset list (internal; swept on engine destroy) */
};

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
 * @param stream Opaque CWIST WebTransport session handle.
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
 * @brief Run the HTTP/3 server event loop.
 */
cwist_error_t cwist_http3_server_loop(int udp_fd,
                                      cwist_http3_context *ctx,
                                      cwist_http3_request_handler_func handler,
                                      void *user_ctx);

/** --- Advanced Features --- */

/**
 * @brief Enable or disable server-side 0-RTT early data on the given context.
 *
 * Early data is disabled by default because 0-RTT payloads are replayable:
 * BoringSSL does not provide replay suppression for QUIC early data, so an
 * attacker can re-send captured 0-RTT packets.  Only opt in when the
 * application can tolerate replays (or relies on the built-in idempotent
 * method guard, see cwist_http3_set_early_data_guard()).
 *
 * The flag is applied immediately to the context's SSL_CTX and stored so the
 * server loop can enforce the replay guard.  Works for both
 * cwist_http3_init_context() and cwist_http3_init_context_ephemeral().
 *
 * Note: enabling early data also requires session resumption; the H3 SSL_CTX
 * is configured with a server session cache and a shared ticket key at
 * context creation so tickets (and thus 0-RTT) work across prefork workers.
 *
 * @param ctx     HTTP/3 context.
 * @param enabled true to accept 0-RTT early data, false to reject it.
 */
void cwist_http3_set_early_data(cwist_http3_context *ctx, bool enabled);

/**
 * @brief Enable or disable the 0-RTT replay guard (default ON when early
 *        data is enabled).
 *
 * When the guard is active, a request that arrived as early data (i.e. while
 * the QUIC handshake is still in progress) is only passed to the application
 * handler if its method is idempotent (GET/HEAD/PUT/DELETE/OPTIONS/TRACE);
 * other methods are answered with 425 Too Early so the client retries after
 * the handshake completes.
 *
 * @param ctx     HTTP/3 context.
 * @param enabled Non-zero to keep the guard, 0 to disable it (replay risk).
 */
void cwist_http3_set_early_data_guard(cwist_http3_context *ctx, int enabled);

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
 * @deprecated Do NOT call this on a request stream. lsquic implements it by
 * emitting a PRIORITY_UPDATE frame on the control stream, which violates
 * RFC 9218 (PRIORITY_UPDATE is only valid for streams the client opened).
 * Strict HTTP/3 stacks (Firefox/neqo) abort the whole connection with
 * H3_FRAME_UNEXPECTED (0x105), surfacing in the browser as a network
 * protocol error. Express response priority with the `Priority` response
 * header (RFC 9218) instead.
 *
 * This function is kept for ABI compatibility but currently always refuses:
 * it logs a warning and returns -1 without touching the stream.
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
 * @param stream  Opaque CWIST WebTransport stream handle.
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
 * @param stream  Opaque CWIST WebTransport stream handle.
 * @param data    Payload to write.
 * @param len     Payload length in bytes.
 * @return Number of bytes buffered, or -1 on error.
 */
ssize_t cwist_webtransport_write(void *stream, const void *data, size_t len);

/**
 * @brief Flush any buffered data on a WebTransport stream.
 *
 * @param stream  Opaque CWIST WebTransport stream handle.
 * @return 0 on success, -1 on error.
 */
int cwist_webtransport_flush(void *stream);

/**
 * @brief Close a WebTransport stream.
 *
 * @param stream  Opaque CWIST WebTransport stream handle.
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
 * @param session  Opaque CWIST WebTransport session handle.
 * @return 0 on success, -1 on failure.
 */
int cwist_webtransport_open_bidi_stream(void *session);

/**
 * @brief Request a new server-initiated unidirectional WebTransport stream.
 *
 * The actual stream is delivered asynchronously via the new-stream handler.
 *
 * @param session  Opaque CWIST WebTransport session handle.
 * @return 0 on success, -1 on failure.
 */
int cwist_webtransport_open_uni_stream(void *session);

/**
 * @brief Send an unreliable WebTransport datagram in session context.
 *
 * @param session Opaque CWIST WebTransport session handle.
 * @param data    Datagram payload.
 * @param len     Payload length in bytes.
 * @return Number of bytes queued, or -1 on error.
 */
ssize_t cwist_webtransport_send_datagram(void *session,
                                         const void *data, size_t len);

/**
 * @brief Return the current maximum WebTransport datagram payload size.
 */
size_t cwist_webtransport_max_datagram_size(void *session);

/**
 * @brief Close a WebTransport session with an application error code.
 */
int cwist_webtransport_close_session(void *session,
                                     uint64_t code,
                                     const char *reason);

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
