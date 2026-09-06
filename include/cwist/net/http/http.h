/**
 * @file http.h
 * @brief HTTP Protocol Definitions and Helpers.
 */

#ifndef __CWIST_HTTP_H__
#define __CWIST_HTTP_H__

#include <cwist/core/sstring/sstring.h>
#include <cwist/sys/err/cwist_err.h>
#include <cwist/net/http/query.h>
#include <cwist/core/db/sql.h>
#include <cwist/sys/app/endpoint_opts.h>
#include <cwist/sys/io/reactor.h>
#include <stdint.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>

long get_cpu_cores(void);
long get_optimal_thread_count(void);

struct cwist_app;
struct cwist_http_async_conn;

/** --- Enums --- */

typedef enum cwist_http_method_t {
    CWIST_HTTP_GET,
    CWIST_HTTP_POST,
    CWIST_HTTP_PUT,
    CWIST_HTTP_DELETE,
    CWIST_HTTP_PATCH,
    CWIST_HTTP_HEAD,
    CWIST_HTTP_OPTIONS,
    CWIST_HTTP_CONNECT,
    CWIST_HTTP_UNKNOWN
} cwist_http_method_t;

typedef enum cwist_http_status_t {
    /* 1xx Informational */
    CWIST_HTTP_CONTINUE = 100,
    CWIST_HTTP_SWITCHING_PROTOCOLS = 101,
    CWIST_HTTP_PROCESSING = 102,
    CWIST_HTTP_EARLY_HINTS = 103,
    /* 2xx Success */
    CWIST_HTTP_OK = 200,
    CWIST_HTTP_CREATED = 201,
    CWIST_HTTP_ACCEPTED = 202,
    CWIST_HTTP_NON_AUTHORITATIVE_INFORMATION = 203,
    CWIST_HTTP_NO_CONTENT = 204,
    CWIST_HTTP_RESET_CONTENT = 205,
    CWIST_HTTP_PARTIAL_CONTENT = 206,
    CWIST_HTTP_MULTI_STATUS = 207,
    CWIST_HTTP_ALREADY_REPORTED = 208,
    CWIST_HTTP_IM_USED = 226,
    /* 3xx Redirection */
    CWIST_HTTP_MULTIPLE_CHOICES = 300,
    CWIST_HTTP_MOVED_PERMANENTLY = 301,
    CWIST_HTTP_FOUND = 302,
    CWIST_HTTP_SEE_OTHER = 303,
    CWIST_HTTP_NOT_MODIFIED = 304,
    CWIST_HTTP_USE_PROXY = 305,
    CWIST_HTTP_TEMPORARY_REDIRECT = 307,
    CWIST_HTTP_PERMANENT_REDIRECT = 308,
    /* 4xx Client Error */
    CWIST_HTTP_BAD_REQUEST = 400,
    CWIST_HTTP_UNAUTHORIZED = 401,
    CWIST_HTTP_PAYMENT_REQUIRED = 402,
    CWIST_HTTP_FORBIDDEN = 403,
    CWIST_HTTP_NOT_FOUND = 404,
    CWIST_HTTP_METHOD_NOT_ALLOWED = 405,
    CWIST_HTTP_NOT_ACCEPTABLE = 406,
    CWIST_HTTP_PROXY_AUTHENTICATION_REQUIRED = 407,
    CWIST_HTTP_REQUEST_TIMEOUT = 408,
    CWIST_HTTP_CONFLICT = 409,
    CWIST_HTTP_GONE = 410,
    CWIST_HTTP_LENGTH_REQUIRED = 411,
    CWIST_HTTP_PRECONDITION_FAILED = 412,
    CWIST_HTTP_CONTENT_TOO_LARGE = 413,
    CWIST_HTTP_PAYLOAD_TOO_LARGE = 413,
    CWIST_HTTP_URI_TOO_LONG = 414,
    CWIST_HTTP_UNSUPPORTED_MEDIA_TYPE = 415,
    CWIST_HTTP_RANGE_NOT_SATISFIABLE = 416,
    CWIST_HTTP_EXPECTATION_FAILED = 417,
    CWIST_HTTP_IM_A_TEAPOT = 418,
    CWIST_HTTP_MISDIRECTED_REQUEST = 421,
    CWIST_HTTP_UNPROCESSABLE_CONTENT = 422,
    CWIST_HTTP_LOCKED = 423,
    CWIST_HTTP_FAILED_DEPENDENCY = 424,
    CWIST_HTTP_TOO_EARLY = 425,
    CWIST_HTTP_UPGRADE_REQUIRED = 426,
    CWIST_HTTP_PRECONDITION_REQUIRED = 428,
    CWIST_HTTP_TOO_MANY_REQUESTS = 429,
    CWIST_HTTP_REQUEST_HEADER_FIELDS_TOO_LARGE = 431,
    CWIST_HTTP_UNAVAILABLE_FOR_LEGAL_REASONS = 451,
    /* 5xx Server Error */
    CWIST_HTTP_INTERNAL_ERROR = 500,
    CWIST_HTTP_INTERNAL_SERVER_ERROR = 500,
    CWIST_HTTP_NOT_IMPLEMENTED = 501,
    CWIST_HTTP_BAD_GATEWAY = 502,
    CWIST_HTTP_SERVICE_UNAVAILABLE = 503,
    CWIST_HTTP_GATEWAY_TIMEOUT = 504,
    CWIST_HTTP_VERSION_NOT_SUPPORTED = 505,
    CWIST_HTTP_VARIANT_ALSO_NEGOTIATES = 506,
    CWIST_HTTP_INSUFFICIENT_STORAGE = 507,
    CWIST_HTTP_LOOP_DETECTED = 508,
    CWIST_HTTP_NOT_EXTENDED = 510,
    CWIST_HTTP_NETWORK_AUTHENTICATION_REQUIRED = 511
} cwist_http_status_t;

/**
 * @brief Return the standard reason phrase for an HTTP status code
 * (e.g. 404 -> "Not Found"). Unknown codes return NULL; applications may
 * also assign any int to a response status_code for non-standard codes.
 */
const char *cwist_http_status_reason(int status);

/** @brief Failure reason reported by the request receive APIs.
 * Distinguishes protocol errors (which deserve an error response before
 * close) from an orderly client disconnect (close quietly). */
typedef enum cwist_http_parse_error_t {
    CWIST_HTTP_PARSE_OK = 0,          /* No error. */
    CWIST_HTTP_PARSE_EOF,             /* Orderly close / no data: close quietly. */
    CWIST_HTTP_PARSE_MALFORMED,       /* 400: bad request-line, Host rules, CL/TE rules. */
    CWIST_HTTP_PARSE_HEADER_OVERFLOW, /* 431: header block exceeds the read buffer. */
    CWIST_HTTP_PARSE_BODY_TOO_LARGE,  /* 413: Content-Length exceeds the body cap. */
    CWIST_HTTP_PARSE_TE_UNSUPPORTED,  /* 501: unsupported transfer coding. */
    CWIST_HTTP_PARSE_EXPECT_FAILED    /* 417: unsupported Expect value. */
} cwist_http_parse_error_t;

/** --- Constants and Limits --- */
#define CWIST_HTTP_MAX_HEADER_SIZE (8 * 1024)
#define CWIST_HTTP_MAX_BODY_SIZE   (10 * 1024 * 1024)
#define CWIST_HTTP_READ_BUFFER_SIZE (16 * 1024)
#define CWIST_HTTP_TIMEOUT_MS      30000
#define CWIST_HTTPS_HANDSHAKE_TIMEOUT_MS 45000  /* Total TLS handshake budget */
#define CWIST_HTTP_HEADERS_TIMEOUT_MS    120000 /* Total header read budget */
#define CWIST_HTTP_BODY_IDLE_TIMEOUT_MS  60000  /* Abort body read after this much silence */
#define CWIST_HTTP2_IDLE_TIMEOUT_MS      300000 /* Default h2 idle budget (env overridable) */
#define CWIST_HTTP_KEEP_ALIVE_TIMEOUT_SEC 15    /* Default keep-alive idle connection timeout in seconds */

/** --- Structures --- */

/** @brief Linked list node for request/response headers. */
typedef struct cwist_http_header_node {
    cwist_sstring *key;
    cwist_sstring *value;
    struct cwist_http_header_node *next;
    bool arena_owned;   ///< True when the node lives in a request arena (freed with the arena).
} cwist_http_header_node;

typedef struct cwist_http_request {
    cwist_http_method_t method;
    cwist_sstring *path;        ///< e.g., "/users/1"
    cwist_sstring *query;       ///< e.g., "active=true" (raw)
    cwist_query_map *query_params; ///< Parsed query parameters.
    cwist_query_map *path_params;  ///< Parsed path parameters (e.g. :id).
    cwist_sstring *version;     ///< e.g., "HTTP/1.1"
    cwist_http_header_node *headers;
    cwist_sstring *body;
    bool keep_alive;
    bool te_chunked_seen;  ///< Parser saw and validated Transfer-Encoding: chunked (HTTP/1.1).
    bool expect_100_seen;  ///< Parser saw and validated Expect: 100-continue (HTTP/1.1).
    int client_fd;
    struct cwist_app *app;  ///< Owning app context (if any).
    cwist_db *db;           ///< Shared database handle from cwist_app.
    bool upgraded;
    uint32_t stream_id;     ///< HTTP/2 or HTTP/3 stream ID (0 for HTTP/1.1).
    void *private_data;     ///< Internal framework use (protocol-specific context).
    void *route_middleware_state; ///< Router middleware chain state (internal).
    size_t content_length;
    cwist_endpoint_opt_t endpoint_opts; ///< Behavior hints for the active endpoint.
    cwist_query_map *flash; ///< Flash messages for this request (one-time read).
    void *session;          ///< cwist_session_t pointer set by session middleware.
    char *csrf_token;       ///< CSRF token populated by csrf middleware.
    void *arena;            ///< Per-request arena for bump-allocated structs (internal).
    void *async_conn;       ///< C1M connection shell when on the reactor path (internal).
} cwist_http_request;

typedef void (*cwist_http_body_cleanup_fn)(const void *ptr, size_t len, void *ctx);

/**
 * @brief HTTP Response Object.
 * Supports standard string body or Zero-Copy pointer body.
 */
typedef struct cwist_http_response {
    cwist_sstring *version;     ///< e.g., "HTTP/1.1"
    cwist_http_status_t status_code;
    cwist_sstring *status_text; ///< e.g., "OK"
    cwist_http_header_node *headers;
    cwist_sstring *body;
    cwist_endpoint_opt_t endpoint_opts; ///< Mirrors req->endpoint_opts.
    
    /// Zero-Copy Pointer Body
    bool is_ptr_body;        ///< If true, body data is read from ptr_body
    const void *ptr_body;    ///< Pointer to external data (e.g., mmap region)
    size_t ptr_body_len;     ///< Length of external data
    cwist_http_body_cleanup_fn ptr_body_cleanup; ///< Optional release hook
    void *ptr_body_cleanup_ctx; ///< User data for release hook

    /// Fast File Streaming
    bool use_file_stream;        ///< True when sendfile/splice path is used.
    int file_stream_fd;          ///< Open descriptor for sendfile.
    size_t file_stream_len;      ///< Total bytes to stream.
    off_t file_stream_offset;    ///< Current offset for sendfile loop.
    bool file_stream_auto_close; ///< Close fd after streaming.
    
    bool keep_alive;
    
    /// Alt-Svc header for HTTP/3 upgrade advertisement
    char *alt_svc;

    void *arena;            ///< Per-response arena for bump-allocated structs (internal).
    bool arena_borrowed;    ///< True when arena is shared (e.g. the request's) and must not be destroyed here.

    /// Deferred-response handoff (internal; see net/http/async.h)
    bool deferred;          ///< Handler deferred the response via cwist_async_defer.
    void *async;            ///< Owning cwist_async while deferred.
} cwist_http_response;

/** --- API Functions --- */

/** @name Request Lifecycle */
/** @{ */
cwist_http_request *cwist_http_request_create(void);
void cwist_http_request_destroy(cwist_http_request *req);
cwist_http_request *cwist_http_parse_request(const char *raw_request); 
cwist_http_request *cwist_http_receive_request(int client_fd, char *read_buf, size_t buf_size, size_t *buf_len, cwist_http_parse_error_t *err_out);
/**
 * @brief Send a minimal HTTP/1.x error response (Connection: close) on a
 * socket, used to answer malformed requests before dropping them.
 * @param msg Plain-text body; NULL uses the status reason phrase.
 */
void cwist_http_send_error_response(int fd, int status, const char *msg);
/** @} */

/** @name Request Data Processing */
/** @{ */
cwist_sstring* cwist_get_client_ip_from_fd(int fd);

/**
 * @brief Format a time_t as an HTTP-date (RFC 7231).
 */
void cwist_http_format_date(time_t t, char *buf, size_t len);

/**
 * @brief Parse an HTTP-date string into a time_t.
 */
time_t cwist_http_parse_date(const char *str);
/** @} */

/** @name Response Lifecycle */
/** @{ */
cwist_http_response *cwist_http_response_create(void);
/**
 * @brief Create a response inside an existing (request) arena.
 * The response is bump-allocated from @p arena and shares its lifetime:
 * cwist_http_response_destroy releases body/header resources but leaves the
 * arena itself to the owner. @p arena must outlive the response.
 */
cwist_http_response *cwist_http_response_create_in_arena(void *arena);
void cwist_http_response_destroy(cwist_http_response *res);

/**
 * @brief Sets a direct pointer for the response body (Zero Copy).
 * Use this when serving large files from memory mapped regions.
 * The pointer must remain valid until the response is sent.
 */
void cwist_http_response_set_body_ptr(cwist_http_response *res, const void *ptr, size_t len);
void cwist_http_response_set_body_ptr_managed(cwist_http_response *res, const void *ptr, size_t len, cwist_http_body_cleanup_fn cleanup, void *ctx);
void cwist_http_response_set_alt_svc(cwist_http_response *res, const char *alt_svc);

cwist_sstring *cwist_http_stringify_response(cwist_http_response *res);
cwist_error_t cwist_http_send_response(int client_fd, cwist_http_response *res);
/**
 * @brief Send only the status line and headers of a response (HEAD replies).
 * Content-Length still reflects the would-be body; body resources are
 * released without being transmitted.
 */
cwist_error_t cwist_http_send_response_head(int client_fd, cwist_http_response *res);
/**
 * @brief Whether the TCP_CORK coalescing layer is active for cleartext
 * HTTP/1.1 responses. Enabled at runtime with CWIST_USE_TCP_CORK=1 (burst
 * size via CWIST_TCP_CORK_BURST, default 256 KiB); always false off-Linux.
 */
bool cwist_tcp_cork_enabled(void);
/**
 * @brief Serialize only the status line and headers into a caller buffer.
 * Used by the TLS send path to stream header block and body separately.
 * @return Number of bytes written.
 */
size_t cwist_http_serialize_headers(cwist_http_response *res, char *buf, size_t buf_size);
cwist_error_t cwist_http_response_send_file(cwist_http_response *res, const char *file_path, const char *content_type_hint, size_t *out_size);
/** @} */

/** @name Header Manipulation */
/** @{ */
cwist_error_t cwist_http_header_add(cwist_http_header_node **head, const char *key, const char *value);
/**
 * @brief Finds a header value by key.
 * @return Raw C-string pointer (NULL if not found).
 */
char *cwist_http_header_get(cwist_http_header_node *head, const char *key);
/**
 * @brief Remove all headers matching a name, honoring per-node ownership
 * (heap vs arena vs borrowed buffers).
 * @return Number of nodes removed.
 */
size_t cwist_http_header_remove(cwist_http_header_node **head, const char *key);
void cwist_http_header_free_all(cwist_http_header_node *head);

/**
 * @brief Add default security headers (CSP, X-Frame-Options, etc.) if missing.
 */
void cwist_http_response_add_security_headers(cwist_http_response *res);
/** @} */

/** @name Helpers */
/** @{ */
const char *cwist_http_method_to_string(cwist_http_method_t method);
cwist_http_method_t cwist_http_string_to_method(const char *method_str);
cwist_http_method_t cwist_http_string_to_method_len(const char *str, size_t len);
/** @} */

/** @name TCP Socket Helpers */
/** @{ */
/** @brief Create an IPv4 socket and perform bind/listen. */
int cwist_make_socket_ipv4(struct sockaddr_in *sockv4, const char *address, uint16_t port, uint16_t backlog);
/** @brief Accept sockets and invoke a handler callback. */
cwist_error_t cwist_accept_socket(int server_fd, struct sockaddr *sockv4, void (*handler_func)(int client_fd, void *ctx), void *ctx);
/** @} */

typedef struct cwist_server_config {
    bool use_forking;     ///< Process per request.
    bool use_threading;   ///< Thread per request.
    bool use_epoll;       ///< Use epoll for accepting.
} cwist_server_config;

cwist_error_t cwist_http_server_loop(int server_fd, cwist_server_config *config, void (*handler)(int, void *), void *ctx);
int headers_have_content_length(cwist_http_header_node *headers);

int cwist_http_pool_init(void);
void cwist_http_pool_limit_core(unsigned int limit);
void cwist_http_pool_submit(int client_fd, void (*handler)(int, void *), void *ctx);
bool cwist_http_pool_rearm_current(int client_fd, void (*handler)(int, void *), void *ctx);
void cwist_http_pool_destroy(void);

/* --- Event-driven (one-shot) connection path for the C1M reactor ---------
 * The classic pool handler parks a worker thread on each keep-alive
 * connection, capping concurrent connections at the thread count.  The async
 * path below never blocks on a read: the callback drains whatever arrived,
 * serves every complete request, and rearms the fd, so one thread can hold
 * hundreds of thousands of mostly-idle connections.
 *
 * Writes still use the bounded poll wait inside cwist_http_send_response
 * (CWIST_HTTP_TIMEOUT_MS), so a slow client can occupy a reactor thread for
 * that budget; true EPOLLOUT write resumption is future work. */
typedef enum {
    CWIST_ASYNC_CLOSE = 0,  /* Close fd and release the connection. */
    CWIST_ASYNC_REARM,      /* Keep the connection; wait for more reads. */
    CWIST_ASYNC_DETACH,     /* Handler took ownership of fd (h2c, upgrades). */
    CWIST_ASYNC_DEFER       /* Response deferred (cwist_async); leave fd and conn untouched. */
} cwist_async_action_t;

typedef cwist_async_action_t (*cwist_async_handler_t)(int fd, struct cwist_http_async_conn *conn);

typedef struct cwist_http_async_conn {
    int fd;
    void *user_ctx;                       /* Owning app context. */
    char *rbuf;                           /* Lazy recv stash; freed while empty. */
    size_t cap;
    size_t len;
    bool virgin;                          /* No bytes seen yet (h2c preface sniff). */
    bool expect_continue_sent;            /* 100 Continue already emitted for the pending request. */
    uint32_t last_active_sec;             /* Monotonic timestamp of last activity (for idle reaping). */
    uint32_t worker_id;                   /* Assigned worker thread index for load tracking. */
    cwist_reactor_t *reactor;             /* Owning reactor (deferred-response completion target). */
    cwist_async_handler_t handler;        /* Connection handler, reused for re-arm after a defer. */
} cwist_http_async_conn_t;

/* Re-arm a connection after a deferred response completed on the reactor
 * thread (keep-alive), reusing the same one-shot event slot model as
 * http_async_event_cb.  On failure the fd is closed and conn released.
 * cwist_http_async_close closes the fd and releases the connection shell. */
bool cwist_http_async_rearm(int client_fd, cwist_reactor_t *reactor, cwist_http_async_conn_t *conn);
void cwist_http_async_close(int client_fd, cwist_http_async_conn_t *conn);

typedef enum {
    CWIST_RECV_OK = 0,
    CWIST_RECV_NEED_MORE,   /* Partial request; rearm and wait. */
    CWIST_RECV_FATAL        /* Protocol error / overflow; close. */
} cwist_recv_status_t;

bool cwist_http_pool_submit_async(int client_fd, cwist_async_handler_t handler, void *ctx);
int cwist_http_async_conn_fill(cwist_http_async_conn_t *conn);
cwist_recv_status_t cwist_http_receive_request_nb(cwist_http_async_conn_t *conn, cwist_http_request **out, cwist_http_parse_error_t *err_out);

extern const int CWIST_CREATE_SOCKET_FAILED;
extern const int CWIST_HTTP_UNAVAILABLE_ADDRESS;
extern const int CWIST_HTTP_BIND_FAILED;
extern const int CWIST_HTTP_SETSOCKOPT_FAILED;
extern const int CWIST_HTTP_LISTEN_FAILED;

#endif
