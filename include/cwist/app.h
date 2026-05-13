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

// Middleware type: receives req, res, and the next stage in the chain
typedef void (*cwist_middleware_func)(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next);

typedef struct cwist_middleware_node {
    cwist_middleware_func func;
    struct cwist_middleware_node *next;
} cwist_middleware_node;

typedef struct cwist_route_node {
    const char *path;
    cwist_http_method_t method;
    cwist_handler_func handler;
    cwist_ws_handler_func ws_handler; // For WS routes
    struct cwist_route_node *next;
} cwist_route_node;

typedef struct cwist_app {
    int port;
    bool use_ssl;
    char *cert_path;
    char *key_path;
    
    // Middlewares
    cwist_middleware_node *middlewares;

    // Simple linked list router for now
    cwist_route_node *routes;
    
    // Error Handling
    cwist_error_handler_func error_handler;

    // Internal contexts
    cwist_https_context *ssl_ctx;
} cwist_app;

// --- API ---

cwist_app *cwist_app_create(void);
void cwist_app_destroy(cwist_app *app);

// Middleware
void cwist_app_use(cwist_app *app, cwist_middleware_func mw);

// Error Handling Configuration
void cwist_app_set_error_handler(cwist_app *app, cwist_error_handler_func handler);



cwist_error_t cwist_app_use_https(cwist_app *app, const char *cert_path, const char *key_path);

// Routing
void cwist_app_get(cwist_app *app, const char *path, cwist_handler_func handler);
void cwist_app_post(cwist_app *app, const char *path, cwist_handler_func handler);
void cwist_app_ws(cwist_app *app, const char *path, cwist_ws_handler_func handler);
// Add other methods as needed

// Start
int cwist_app_listen(cwist_app *app, int port);

#endif
