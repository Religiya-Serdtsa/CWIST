/**
 * @file app.h
 * @brief Core Application Structure and Lifecycle Management.
 */

#ifndef __CWIST_APP_H__
#define __CWIST_APP_H__

#include <cwist/net/http/http.h>
#include <cwist/net/http/https.h>
#include <cwist/core/db/sql.h>
#include <cwist/sys/err/cwist_err.h>
#include <cwist/core/macros.h>
#include <cwist/sys/app/big_dumb_reply.h>
#include <ttak/mem_tree/mem_tree.h>

#include <cwist/net/websocket/websocket.h>

/**
 * @brief Function pointer type for HTTP route handlers.
 * @param req Pointer to the HTTP request object.
 * @param res Pointer to the HTTP response object.
 */
typedef void (*cwist_handler_func)(cwist_http_request *req, cwist_http_response *res);

/**
 * @brief Function pointer type for WebSocket handlers.
 * @param ws Pointer to the WebSocket context.
 */
typedef void (*cwist_ws_handler_func)(cwist_websocket *ws);

/**
 * @brief Function pointer type for error handlers.
 */
typedef void (*cwist_error_handler_func)(cwist_http_request *req, cwist_http_response *res, cwist_http_status_t status);

/**
 * @brief Middleware type that receives req/res pair and the next stage in the chain.
 */
typedef void (*cwist_middleware_func)(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next);

/**
 * @brief Linked list node for middleware chain.
 */
typedef struct cwist_middleware_node {
    cwist_middleware_func func;
    struct cwist_middleware_node *next;
} cwist_middleware_node;

typedef struct cwist_route_table cwist_route_table;
typedef struct cwist_static_dir cwist_static_dir;

/**
 * @brief Main Application Context.
 * 
 * Manages routing, middleware, database connections, memory pools,
 * and caching strategies (BDR).
 */
typedef struct cwist_app {
    int port;
    bool use_ssl;
    char *cert_path;
    char *key_path;
    
    cwist_middleware_node *middlewares; ///< Head of the middleware chain.

    cwist_route_table *router; ///< Router definition.
    cwist_static_dir *static_dirs; ///< Static directory mappings.
    
    cwist_error_handler_func error_handler; ///< Error handling callback.

    cwist_https_context *ssl_ctx; ///< SSL context when TLS is enabled.
    cwist_db *db; ///< Shared database handle.
    char *db_path; ///< Database path (if set).
    bool nuke_enabled; ///< True when NUKE DB integration is active.

    /** @brief Max memory space for static file pool (0 = auto-detected * 2) */
    size_t max_mem_space;
    /** @brief Memory manager for static asset caching and hot-reloading */
    struct cwist_fix_server_mem *mem_manager;
    
    /** @brief Big Dumb Reply context for auto-caching high-latency endpoints */
    cwist_bdr_t *bdr_ctx;
} cwist_app;

/** --- Memory Management --- */

/**
 * @brief Represents a file loaded into the fixed memory pool.
 */
typedef struct cwist_file_t {
    char *path;       ///< Relative path (URL path)
    char *fs_path;    ///< Full filesystem path
    void *data;       ///< Pointer to memory-tracked file contents
    size_t size;      ///< Size of the file in bytes
    time_t last_mod;  ///< Last modification time
    ttak_mem_node_t *node; ///< Tracking node for libttak lifecycle
} cwist_file_t;

/**
 * @brief Fixed Server Memory Manager.
 * 
 * Pre-allocates a large contiguous block of memory to serve static files
 * via Zero-Copy pointer passing. Supports hot-reloading on file change.
 */
typedef struct cwist_fix_server_mem {
    size_t total_capacity;     ///< Total capacity (defaults to sum of files * 2)
    size_t current_used;       ///< Bytes accounted for by active files
    
    cwist_file_t *files;       ///< Array of tracked files
    size_t file_count;
    size_t files_capacity;     ///< Capacity of the files array

    uint64_t retire_grace_ns;  ///< Delay before recycling replaced buffers
    ttak_mem_tree_t file_tree; ///< Lifetime tracking tree for file buffers

    pthread_mutex_t lock;
    pthread_t watcher_thread;
    bool watcher_running;
    int check_interval_ms;
} cwist_fix_server_mem;

/** --- API --- */

/**
 * @brief Creates a new CWIST application instance.
 * @return Pointer to the allocated app, or NULL on failure.
 */
cwist_app *cwist_app_create(void);

/**
 * @brief Destroys the application and frees all resources.
 * @param app Pointer to the app to destroy.
 */
void cwist_app_destroy(cwist_app *app);

/**
 * @brief Sets the maximum memory space for the static file pool.
 * @param app Pointer to the app.
 * @param size Size in bytes (Use macros like CWIST_MIB(64)).
 */
void cwist_app_set_max_memspace(cwist_app *app, size_t size);

/** @name Middleware */
/** @{ */
void cwist_app_use(cwist_app *app, cwist_middleware_func mw);
/** @} */

/** @name Error Handling Configuration */
/** @{ */
void cwist_app_set_error_handler(cwist_app *app, cwist_error_handler_func handler);
/** @} */

/**
 * @brief Configures the Big Dumb Reply guardrails.
 * @param app Target app.
 * @param max_bytes Maximum bytes to keep in RAM (0 = keep default).
 * @param max_entry_age_sec Retire cached replies older than this (<=0 keeps default).
 * @param revalidate_hits Force refresh after this many hits (0 = keep default).
 */
void cwist_app_configure_bdr(cwist_app *app, size_t max_bytes, time_t max_entry_age_sec, uint64_t revalidate_hits);

cwist_error_t cwist_app_use_https(cwist_app *app, const char *cert_path, const char *key_path);
cwist_error_t cwist_app_use_db(cwist_app *app, const char *db_path);
cwist_error_t cwist_app_use_nuke_db(cwist_app *app, const char *db_path, int sync_interval_ms);
cwist_db *cwist_app_get_db(cwist_app *app);

/** @name Routing */
/** @{ */
/**
 * @brief Registers a GET route handler.
 */
void cwist_app_get(cwist_app *app, const char *path, cwist_handler_func handler);
void cwist_app_post(cwist_app *app, const char *path, cwist_handler_func handler);
void cwist_app_ws(cwist_app *app, const char *path, cwist_ws_handler_func handler);

/**
 * @brief Serves a directory of static files at a URL prefix.
 * Files are loaded into the fixed memory pool for Zero-Copy serving.
 * @param app Pointer to the app.
 * @param url_prefix URL prefix (e.g., "/static").
 * @param directory Local filesystem path.
 * @note Additional method helpers can be added as needed.
 */
cwist_error_t cwist_app_static(cwist_app *app, const char *url_prefix, const char *directory);
/** @} */

/** @name Startup */
/** @{ */
int cwist_app_listen(cwist_app *app, int port);
/** @} */

#endif
