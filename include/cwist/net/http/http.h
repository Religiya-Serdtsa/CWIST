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
#include <netinet/in.h>
#include <sys/socket.h>

struct cwist_app;

/** --- Enums --- */

typedef enum cwist_http_method_t {
    CWIST_HTTP_GET,
    CWIST_HTTP_POST,
    CWIST_HTTP_PUT,
    CWIST_HTTP_DELETE,
    CWIST_HTTP_PATCH,
    CWIST_HTTP_HEAD,
    CWIST_HTTP_OPTIONS,
    CWIST_HTTP_UNKNOWN
} cwist_http_method_t;

typedef enum cwist_http_status_t {
    CWIST_HTTP_OK = 200,
    CWIST_HTTP_CREATED = 201,
    CWIST_HTTP_NO_CONTENT = 204,
    CWIST_HTTP_BAD_REQUEST = 400,
    CWIST_HTTP_UNAUTHORIZED = 401,
    CWIST_HTTP_FORBIDDEN = 403,
    CWIST_HTTP_NOT_FOUND = 404,
    CWIST_HTTP_INTERNAL_ERROR = 500,
    CWIST_HTTP_NOT_IMPLEMENTED = 501
} cwist_http_status_t;

/** --- Constants and Limits --- */
#define CWIST_HTTP_MAX_HEADER_SIZE (8 * 1024)
#define CWIST_HTTP_MAX_BODY_SIZE   (10 * 1024 * 1024)
#define CWIST_HTTP_READ_BUFFER_SIZE (16 * 1024)
#define CWIST_HTTP_TIMEOUT_MS      5000

/** --- Structures --- */

/** @brief Linked list node for request/response headers. */
typedef struct cwist_http_header_node {
    cwist_sstring *key;
    cwist_sstring *value;
    struct cwist_http_header_node *next;
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
    int client_fd;
    struct cwist_app *app;  ///< Owning app context (if any).
    cwist_db *db;           ///< Shared database handle from cwist_app.
    bool upgraded;
    void *private_data; ///< Internal framework use.
    size_t content_length;
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
    
    /// Zero-Copy Pointer Body
    bool is_ptr_body;        ///< If true, body data is read from ptr_body
    const void *ptr_body;    ///< Pointer to external data (e.g., mmap region)
    size_t ptr_body_len;     ///< Length of external data
    cwist_http_body_cleanup_fn ptr_body_cleanup; ///< Optional release hook
    void *ptr_body_cleanup_ctx; ///< User data for release hook
    
    bool keep_alive;
} cwist_http_response;

/** --- API Functions --- */

/** @name Request Lifecycle */
/** @{ */
cwist_http_request *cwist_http_request_create(void);
void cwist_http_request_destroy(cwist_http_request *req);
cwist_http_request *cwist_http_parse_request(const char *raw_request); 
cwist_http_request *cwist_http_receive_request(int client_fd, char *read_buf, size_t buf_size, size_t *buf_len);
/** @} */

/** @name Request Data Processing */
/** @{ */
cwist_sstring* cwist_get_client_ip_from_fd(int fd);
/** @} */

/** @name Response Lifecycle */
/** @{ */
cwist_http_response *cwist_http_response_create(void);
void cwist_http_response_destroy(cwist_http_response *res);

/**
 * @brief Sets a direct pointer for the response body (Zero Copy).
 * Use this when serving large files from memory mapped regions.
 * The pointer must remain valid until the response is sent.
 */
void cwist_http_response_set_body_ptr(cwist_http_response *res, const void *ptr, size_t len);
void cwist_http_response_set_body_ptr_managed(cwist_http_response *res, const void *ptr, size_t len, cwist_http_body_cleanup_fn cleanup, void *ctx);

cwist_sstring *cwist_http_stringify_response(cwist_http_response *res);
cwist_error_t cwist_http_send_response(int client_fd, cwist_http_response *res);
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
void cwist_http_header_free_all(cwist_http_header_node *head);
/** @} */

/** @name Helpers */
/** @{ */
const char *cwist_http_method_to_string(cwist_http_method_t method);
cwist_http_method_t cwist_http_string_to_method(const char *method_str);
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

#endif

extern const int CWIST_CREATE_SOCKET_FAILED;
extern const int CWIST_HTTP_UNAVAILABLE_ADDRESS;
extern const int CWIST_HTTP_BIND_FAILED;
extern const int CWIST_HTTP_SETSOCKOPT_FAILED;
extern const int CWIST_HTTP_LISTEN_FAILED;
