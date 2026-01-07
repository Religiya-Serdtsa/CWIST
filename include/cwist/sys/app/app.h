#ifndef __CWIST_APP_H__
#define __CWIST_APP_H__

#include <cwist/net/http/http.h>
#include <cwist/net/http/https.h>
#include <cwist/core/db/sql.h>
#include <cwist/sys/err/cwist_err.h>
#include <cwist/core/macros.h>

#include <cwist/net/websocket/websocket.h>

typedef void (*cwist_handler_func)(cwist_http_request *req, cwist_http_response *res);
typedef void (*cwist_ws_handler_func)(cwist_websocket *ws);
typedef void (*cwist_error_handler_func)(cwist_http_request *req, cwist_http_response *res, cwist_http_status_t status);

// Middleware type: receives req, res, and the next stage in the chain
typedef void (*cwist_middleware_func)(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next);

typedef struct cwist_middleware_node {
    cwist_middleware_func func;
    struct cwist_middleware_node *next;
} cwist_middleware_node;

typedef struct cwist_route_table cwist_route_table;
typedef struct cwist_static_dir cwist_static_dir;

typedef struct cwist_app {
    int port;
    bool use_ssl;
    char *cert_path;
    char *key_path;
    
    // Middlewares
    cwist_middleware_node *middlewares;

    cwist_route_table *router;
    cwist_static_dir *static_dirs;
    
    // Error Handling
    cwist_error_handler_func error_handler;

    // Internal contexts
    cwist_https_context *ssl_ctx;
    cwist_db *db;
    char *db_path;
} cwist_app;

// --- API ---

cwist_app *cwist_app_create(void);
void cwist_app_destroy(cwist_app *app);

// Middleware
void cwist_app_use(cwist_app *app, cwist_middleware_func mw);

// Error Handling Configuration
void cwist_app_set_error_handler(cwist_app *app, cwist_error_handler_func handler);

cwist_error_t cwist_app_use_https(cwist_app *app, const char *cert_path, const char *key_path);
cwist_error_t cwist_app_use_db(cwist_app *app, const char *db_path);
cwist_db *cwist_app_get_db(cwist_app *app);

// Routing
void cwist_app_get(cwist_app *app, const char *path, cwist_handler_func handler);
void cwist_app_post(cwist_app *app, const char *path, cwist_handler_func handler);
void cwist_app_ws(cwist_app *app, const char *path, cwist_ws_handler_func handler);
cwist_error_t cwist_app_static(cwist_app *app, const char *url_prefix, const char *directory);
// Add other methods as needed

// Start
int cwist_app_listen(cwist_app *app, int port);

#endif
