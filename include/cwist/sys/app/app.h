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

/* Forward declaration for auto-mounted RDBMS runtime */
struct cwist_rdbms_runtime;

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
 * @brief Callback function type for handling WebTransport sessions over HTTP/3.
 *
 * @param req    Parsed HTTP request object (CONNECT with :protocol=webtransport).
 * @param res    HTTP response object to be populated (e.g., 200 OK to accept).
 * @param stream Opaque lsquic_stream_t pointer for the WebTransport session.
 */
typedef void (*cwist_webtransport_handler_func)(cwist_http_request *req,
                                                 cwist_http_response *res,
                                                 void *stream);

typedef struct cwist_error_handler_entry {
    cwist_http_status_t status_code;
    cwist_error_handler_func handler;
    struct cwist_error_handler_entry *next;
} cwist_error_handler_entry;

/** @brief RDBMS provider auto-detection result */
typedef enum cwist_rdbms_provider {
    CWIST_RDBMS_NONE = 0,
    CWIST_RDBMS_POSTGRES,
    CWIST_RDBMS_MYSQL,
    CWIST_RDBMS_MARIADB,
} cwist_rdbms_provider_t;

/** @brief Mounted RDBMS runtime state */
typedef struct cwist_rdbms_runtime {
    cwist_rdbms_provider_t provider; /**< Detected provider */
    int port;                        /**< Target TCP port */
    char *host;                      /**< Target host (owned) */
    bool ready;                      /**< True when runtime is usable */
} cwist_rdbms_runtime;

/**
 * @brief Probe a TCP port for an RDBMS wire protocol.
 * @param port TCP port to probe.
 * @return Detected provider, or CWIST_RDBMS_UNKNOWN.
 */
cwist_rdbms_provider_t cwist_rdbms_probe_port(int port);

/**
 * @brief Mount an RDBMS runtime for the given provider and port.
 * @param app      Application context.
 * @param provider Detected provider.
 * @param port     Target port.
 * @return true on success, false on failure.
 */
bool cwist_rdbms_mount_runtime(cwist_app *app, cwist_rdbms_provider_t provider, int port);

/**
 * @brief Middleware type that receives req/res pair and the next stage in the chain.
 */
typedef void (*cwist_middleware_func)(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next);
typedef void (*cwist_https_request_handler_func)(cwist_https_connection *conn, void *ctx);

/**
 * @brief Linked list node for middleware chain.
 */
typedef struct cwist_middleware_node {
    cwist_middleware_func func;
    struct cwist_middleware_node *next;
} cwist_middleware_node;

typedef struct cwist_route_table cwist_route_table;
typedef struct cwist_static_dir cwist_static_dir;
typedef struct cwist_config cwist_config;
typedef struct cwist_logger cwist_logger;

/**
 * @brief Main Application Context.
 * 
 * Manages routing, middleware, database connections, memory pools,
 * and caching strategies (BDR).
 */
typedef struct cwist_app {
    int port;
    bool use_ssl;
    bool use_http2;   ///< Cleartext HTTP/2 (h2c)
    bool use_http3;   ///< Ephemeral (Zero-config) HTTP/3
    bool use_https2;  ///< TLS HTTP/2 (h2)
    bool use_https3;  ///< TLS HTTP/3
    char *cert_path;
    char *key_path;
    cwist_https_request_handler_func https_request_handler;
    
    cwist_middleware_node *middlewares; ///< Head of the middleware chain.

    cwist_route_table *router; ///< Router definition.
    cwist_static_dir *static_dirs; ///< Static directory mappings.
    
    cwist_error_handler_func error_handler; ///< Global fallback error handler.
    cwist_error_handler_entry *error_handlers; ///< Per-status-code error handlers.

    struct cwist_config *config; ///< Application configuration.
    struct cwist_logger *logger; ///< Application logger.

    cwist_https_context *ssl_ctx; ///< SSL context when TLS is enabled.
    struct cwist_http3_context *h3_ctx; ///< HTTP/3 QUIC Context.
    cwist_db *db; ///< Shared database handle.
    char *db_path; ///< Database path (if set).
    bool nuke_enabled; ///< True when NUKE DB integration is active.

    /** @brief Max memory space for static file pool (0 = auto-detected * 2) */
    size_t max_mem_space;
    /** @brief Memory manager for static asset caching and hot-reloading */
    struct cwist_fix_server_mem *mem_manager;
    
    /** @brief Big Dumb Reply context for auto-caching high-latency endpoints */
    cwist_bdr_t *bdr_ctx;

    /** @brief Mounted RDBMS runtime (auto-detected PostgreSQL/MySQL/MariaDB) */
    struct cwist_rdbms_runtime *rdbms;

    /** @brief PQC Layer enabled flag */
    bool pqc_layer_enabled;
    /** @brief Explicit TLS groups override (NULL = automatic) */
    char *tls_groups;

    /** @brief WebTransport session handler */
    cwist_webtransport_handler_func wt_handler;
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
void cwist_app_register_error_handler(cwist_app *app, cwist_http_status_t status, cwist_error_handler_func handler);
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
cwist_error_t cwist_app_use_https2(cwist_app *app, bool enabled);
cwist_error_t cwist_app_use_https3(cwist_app *app, bool enabled);

/**
 * @brief Enable HTTP/2 cleartext (h2c) over standard TCP.
 * @param app Application context.
 * @param enabled True to enable h2c.
 * @return cwist_error_t Status.
 */
cwist_error_t cwist_app_use_http2(cwist_app *app, bool enabled);

/**
 * @brief Enable HTTP/3 (QUIC) without manual SSL configuration.
 * Auto-generates an ephemeral self-signed certificate internally.
 * @param app Application context.
 * @param enabled True to enable zero-config HTTP/3.
 * @return cwist_error_t Status.
 */
cwist_error_t cwist_app_use_http3(cwist_app *app, bool enabled);

/**
 * @brief Enable WebTransport over HTTP/3 on the application.
 *
 * This registers a WebTransport session handler.  The application must
 * also enable HTTP/3 (via cwist_app_use_http3 or cwist_app_use_https3)
 * for WebTransport to function.
 *
 * @param app     Application context.
 * @param handler WebTransport session handler.
 */
void cwist_app_use_webtransport(cwist_app *app, cwist_webtransport_handler_func handler);

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

cwist_error_t cwist_app_use_db(cwist_app *app, const char *db_path);
cwist_error_t cwist_app_use_nuke_db(cwist_app *app, const char *db_path, int sync_interval_ms);
cwist_db *cwist_app_get_db(cwist_app *app);

/**
 * @brief Auto-detect and mount an RDBMS runtime by probing a TCP port.
 *
 * Connects to 127.0.0.1:@p port, probes the wire protocol, detects the
 * provider (PostgreSQL, MySQL, or MariaDB), and mounts a data runtime.
 *
 * @param app  Application context.
 * @param port Target TCP port for RDBMS probe.
 * @return true when provider detected and runtime mounted; false otherwise.
 */
bool cwist_app_auto_rdbms(cwist_app *app, int port);

#define cwist_use_https2(enabled) cwist_app_use_https2((app), (enabled))
#define cwist_use_https3(enabled) cwist_app_use_https3((app), (enabled))
#define cwist_use_http2(enabled) cwist_app_use_http2((app), (enabled))
#define cwist_use_http3(enabled) cwist_app_use_http3((app), (enabled))

/** @name Routing */
/** @{ */
/**
 * @brief Registers a GET route handler.
 */
void cwist_app_get(cwist_app *app, const char *path, cwist_handler_func handler);
void cwist_app_post(cwist_app *app, const char *path, cwist_handler_func handler);
void cwist_app_ws(cwist_app *app, const char *path, cwist_ws_handler_func handler);
void cwist_app_get_opt(cwist_app *app, const char *path, cwist_handler_func handler, cwist_endpoint_opt_t opts);
void cwist_app_post_opt(cwist_app *app, const char *path, cwist_handler_func handler, cwist_endpoint_opt_t opts);
void cwist_app_ws_opt(cwist_app *app, const char *path, cwist_ws_handler_func handler, cwist_endpoint_opt_t opts);

void cwist_app_enable_metrics(cwist_app *app);
void cwist_app_enable_healthz(cwist_app *app);

void cwist_app_get_named(cwist_app *app, const char *path, const char *name, cwist_handler_func handler);
void cwist_app_post_named(cwist_app *app, const char *path, const char *name, cwist_handler_func handler);
char *cwist_url_for(cwist_app *app, const char *name, cwist_query_map *params);

/**
 * @brief Serves a directory of static files at a URL prefix.
 * Files are loaded into the fixed memory pool for Zero-Copy serving.
 * @param app Pointer to the app.
 * @param url_prefix URL prefix (e.g., "/static").
 * @param directory Local filesystem path.
 * @note Additional method helpers can be added as needed.
 */
cwist_error_t cwist_app_static(cwist_app *app, const char *url_prefix, const char *directory);

/**
 * @brief Serves a directory of static files at a URL prefix with custom Cache-Control.
 * Files are loaded into the fixed memory pool for Zero-Copy serving.
 * @param app Pointer to the app.
 * @param url_prefix URL prefix (e.g., "/static").
 * @param directory Local filesystem path.
 * @param cache_control Cache-Control directive string (e.g., "public, max-age=31536000, immutable").
 *        Pass NULL to use the default "public, max-age=3600".
 */
cwist_error_t cwist_app_static_with_cache(cwist_app *app, const char *url_prefix, const char *directory, const char *cache_control);
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
    unsigned short ports[CWIST_MULTIPORT_MAX_PORTS]; ///< Additional TCP ports.
    size_t count; ///< Number of valid entries in ports.
    bool valid; ///< False when construction failed, e.g. too many ports.
} cwist_multiport_t;

/**
 * @brief Create a counted multiport descriptor from an explicit pointer and length.
 * @param ports Source port array.
 * @param count Number of elements in ports.
 * @return Counted descriptor accepted by cwist_app_multiport().
 */
cwist_multiport_t cwist_create_multiport_from_array(const unsigned short *ports, size_t count);

/**
 * @brief Create a counted multiport descriptor from a real C array.
 * @param ports Real C array, not a decayed pointer.
 */
#define cwist_create_multiport(ports) cwist_create_multiport_from_array((ports), sizeof(ports) / sizeof((ports)[0]))

void cwist_app_http_handler(int client_fd, void *ctx);
int cwist_app_listen(cwist_app *app, int port);

/**
 * @brief Start one app facade across a public port and counted backend port list.
 * @param app_ref Address of the cwist_app pointer (use: cwist_app_multiport(&app, 443, ports)).
 * @param public_port Primary public TCP port.
 * @param ports Additional TCP ports created by cwist_create_multiport().
 * @return 0 after graceful shutdown, or -1 on validation/bind failure.
 */
int cwist_app_multiport(cwist_app **app_ref, unsigned short public_port, cwist_multiport_t ports);

/**
 * @brief Detach one additional multiport port into its own tunable application.
 * @param app_ref Address of the root cwist_app pointer.
 * @param port Additional port to detach. The public/default port is rejected by cwist_app_multiport().
 * @return Detached sub-application for per-port tuning, or NULL on allocation failure.
 */
cwist_app *cwist_multiport_get_app(cwist_app **app_ref, unsigned short port);
/** @} */

/**
 * @brief Dispatch a request internally through the app's router and middleware.
 * Used by the test client and advanced integrations.
 */
void cwist_app_dispatch(cwist_app *app, cwist_http_request *req, cwist_http_response *res);

#endif
