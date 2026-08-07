/** @file app.h
 * @brief app.h interface.
 */
#ifndef __CWIST_APP_H__
#define __CWIST_APP_H__

#include <cwist/http.h>
#include <cwist/https.h>
#include <cwist/net/grpc/grpc.h>
#include <cwist/err/cwist_err.h>
#include <cwist/macros.h>

#include <cwist/websocket.h>
#include <cwist/sys/app/endpoint_opts.h>

/* Forward declaration for the database handle */
typedef struct cwist_db cwist_db;

typedef void (*cwist_handler_func)(cwist_http_request *req, cwist_http_response *res);
typedef void (*cwist_ws_handler_func)(cwist_websocket *ws);
typedef void (*cwist_error_handler_func)(cwist_http_request *req, cwist_http_response *res, cwist_http_status_t status);

/**
 * @brief Callback function type for handling WebTransport sessions over HTTP/3.
 *
 * @param req    Parsed HTTP request object (CONNECT with :protocol=webtransport).
 * @param res    HTTP response object to be populated (e.g., 200 OK to accept).
 * @param stream Opaque lsquic_stream_t pointer for the WebTransport session.
 */
typedef void (*cwist_webtransport_handler_func)(cwist_http_request *req,
                                                 cwist_http_response *res,
                                                 void *stream);

/// Middleware type: receives req, res, and the next stage in the chain
typedef void (*cwist_middleware_func)(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next);

typedef struct cwist_middleware_node {
    cwist_middleware_func func;
    struct cwist_middleware_node *next;
} cwist_middleware_node;

typedef struct cwist_route_node {
    const char *path;
    cwist_http_method_t method;
    cwist_handler_func handler;
    cwist_ws_handler_func ws_handler; ///< For WS routes
    struct cwist_route_node *next;
} cwist_route_node;

typedef struct cwist_app {
    int port;
    bool use_ssl;
    char *cert_path;
    char *key_path;
    
    /// Middlewares
    cwist_middleware_node *middlewares;

    /// Simple linked list router for now
    cwist_route_node *routes;
    
    /// Error Handling
    cwist_error_handler_func error_handler;

    /// Internal contexts
    cwist_https_context *ssl_ctx;

    /// PQC Layer enabled flag
    bool pqc_layer_enabled;
    /// Explicit TLS groups override (NULL = automatic)
    char *tls_groups;

    /// WebTransport session handler
    cwist_webtransport_handler_func wt_handler;

    /// Mounted RDBMS runtime (opaque)
    void *rdbms;
} cwist_app;

/** @name Lifecycle */
/** @{ */

/**
 * @brief Create a new CWIST application.
 */
cwist_app *cwist_app_create(void);

/**
 * @brief Destroy the application.
 */
void cwist_app_destroy(cwist_app *app);

/** @} */

/** @name Middleware */
/** @{ */

/**
 * @brief Register global middleware.
 */
void cwist_app_use(cwist_app *app, cwist_middleware_func mw);

/** @} */

/** @name Error Handling */
/** @{ */

/**
 * @brief Set the global error handler.
 */
void cwist_app_set_error_handler(cwist_app *app, cwist_error_handler_func handler);

/** @} */

/** @name HTTPS */
/** @{ */

/**
 * @brief Enable HTTPS with a certificate.
 */
cwist_error_t cwist_app_use_https(cwist_app *app, const char *cert_path, const char *key_path);

/**
 * @brief Enable Post-Quantum Cryptography (PQC) hybrid key exchange layer.
 * @param app Application context.
 * @param enabled True to enable PQC hybrid TLS groups and force TLS 1.3.
 */
void cwist_app_use_pqc_layer(cwist_app *app, bool enabled);

/**
 * @brief Override TLS groups explicitly.
 * @param app Application context.
 * @param groups Colon-separated group list (e.g., "X25519MLKEM768:X25519:P-256").
 *        Pass NULL to reset to automatic selection.
 */
void cwist_app_set_tls_groups(cwist_app *app, const char *groups);

/**
 * @brief Enable WebTransport over HTTP/3 on the application.
 *
 * The application must also enable HTTP/3 for WebTransport to function.
 *
 * @param app     Application context.
 * @param handler WebTransport session handler.
 */
void cwist_app_use_webtransport(cwist_app *app, cwist_webtransport_handler_func handler);

/**
 * @brief Attach a SQLite database to the application.
 *
 * @param app     Application context.
 * @param db_path Database file path (":memory:" for RAM-only).
 */
void cwist_app_use_db(cwist_app *app, const char *db_path);

/**
 * @brief Attach a Nuke DB (in-memory SQLite with disk sync).
 *
 * @param app             Application context.
 * @param db_path         Database file path.
 * @param sync_interval_ms Auto-sync interval in milliseconds.
 */
cwist_error_t cwist_app_use_nuke_db(cwist_app *app, const char *db_path, int sync_interval_ms);

/**
 * @brief Retrieve the shared database handle attached to the app.
 *
 * @param app Application context.
 * @return Pointer to the cwist_db handle, or NULL if none attached.
 */
cwist_db *cwist_app_get_db(cwist_app *app);

/**
 * @brief Auto-detect and mount an RDBMS runtime by probing a TCP port.
 *
 * @param app  Application context.
 * @param port Target TCP port for RDBMS probe.
 * @return true when provider detected and runtime mounted; false otherwise.
 */
bool cwist_app_auto_rdbms(cwist_app *app, int port);

/** @{ */

/** @name Routing */
/** @{ */

/**
 * @brief Register a GET route.
 */
void cwist_app_get(cwist_app *app, const char *path, cwist_handler_func handler);

/**
 * @brief Register a POST route.
 */
void cwist_app_post(cwist_app *app, const char *path, cwist_handler_func handler);

/**
 * @brief Register a WebSocket route.
 */
void cwist_app_ws(cwist_app *app, const char *path, cwist_ws_handler_func handler);

/**
 * @brief Register a GET route with behavior options.
 */
void cwist_app_get_opt(cwist_app *app, const char *path, cwist_handler_func handler, cwist_endpoint_opt_t opts);

/**
 * @brief Register a POST route with behavior options.
 */
void cwist_app_post_opt(cwist_app *app, const char *path, cwist_handler_func handler, cwist_endpoint_opt_t opts);

/**
 * @brief Register a PUT route with behavior options.
 */
void cwist_app_put_opt(cwist_app *app, const char *path, cwist_handler_func handler, cwist_endpoint_opt_t opts);

/**
 * @brief Register a DELETE route with behavior options.
 */
void cwist_app_delete_opt(cwist_app *app, const char *path, cwist_handler_func handler, cwist_endpoint_opt_t opts);

/**
 * @brief Register a PATCH route with behavior options.
 */
void cwist_app_patch_opt(cwist_app *app, const char *path, cwist_handler_func handler, cwist_endpoint_opt_t opts);

/**
 * @brief Register a WebSocket route with behavior options.
 */
void cwist_app_ws_opt(cwist_app *app, const char *path, cwist_ws_handler_func handler, cwist_endpoint_opt_t opts);

/**
 * @brief Enable Prometheus /metrics endpoint.
 */
void cwist_app_enable_metrics(cwist_app *app);

/**
 * @brief Enable health check endpoints (/healthz, /live, /ready).
 */
void cwist_app_enable_healthz(cwist_app *app);

/**
 * @brief Serve a directory of static files at a URL prefix.
 */
cwist_error_t cwist_app_static(cwist_app *app, const char *url_prefix, const char *directory);

/**
 * @brief Enable Prometheus /metrics endpoint.
 */
void cwist_app_enable_metrics(cwist_app *app);

/**
 * @brief Enable health check endpoints (/healthz, /live, /ready).
 */
void cwist_app_enable_healthz(cwist_app *app);

/** @} */

/** @name Startup */
/** @{ */

#ifndef CWIST_MULTIPORT_MAX_PORTS
#define CWIST_MULTIPORT_MAX_PORTS 64
#endif

/**
 * @brief Counted multiport descriptor created from a normal C array.
 */
typedef struct cwist_multiport_t {
    unsigned short ports[CWIST_MULTIPORT_MAX_PORTS];
    size_t count;
    bool valid;
} cwist_multiport_t;

/**
 * @brief Create a counted multiport descriptor from an explicit pointer and length.
 */
cwist_multiport_t cwist_create_multiport_from_array(const unsigned short *ports, size_t count);

/**
 * @brief Create a counted multiport descriptor from a real C array.
 */
#define cwist_create_multiport(ports) cwist_create_multiport_from_array((ports), sizeof(ports) / sizeof((ports)[0]))

/**
 * @brief Start listening on a port.
 */
int cwist_app_listen(cwist_app *app, int port);

/**
 * @brief Start one app facade across a public port and counted backend port list.
 */
int cwist_app_multiport(cwist_app **app_ref, unsigned short public_port, cwist_multiport_t ports);

/**
 * @brief Detach one additional multiport port into its own tunable application.
 */
cwist_app *cwist_multiport_get_app(cwist_app **app_ref, unsigned short port);

/** @} */

#endif
