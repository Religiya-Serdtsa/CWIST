/** @file app.h
 * @brief app.h interface.
 */
#ifndef __CWIST_APP_H__
#define __CWIST_APP_H__

#include <cwist/http.h>
#include <cwist/https.h>
#include <cwist/err/cwist_err.h>
#include <cwist/macros.h>

#include <cwist/websocket.h>

typedef void (*cwist_handler_func)(cwist_http_request *req, cwist_http_response *res);
typedef void (*cwist_ws_handler_func)(cwist_websocket *ws);
typedef void (*cwist_error_handler_func)(cwist_http_request *req, cwist_http_response *res, cwist_http_status_t status);

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

/** @} */

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
