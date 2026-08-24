/** @file http.h
 * @brief http.h interface.
 */
#ifndef __CWIST_HTTP_H__
#define __CWIST_HTTP_H__

#include <cwist/sstring.h>
#include <cwist/err/cwist_err.h>
#include <cwist/query.h>
#include <stdint.h>
#include <netinet/in.h>
#include <sys/socket.h>

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
#define CWIST_HTTP_TIMEOUT_MS      30000

/** @brief Failure reason reported by the request receive APIs. */
typedef enum cwist_http_parse_error_t {
    CWIST_HTTP_PARSE_OK = 0,
    CWIST_HTTP_PARSE_EOF,
    CWIST_HTTP_PARSE_MALFORMED,
    CWIST_HTTP_PARSE_HEADER_OVERFLOW,
    CWIST_HTTP_PARSE_BODY_TOO_LARGE,
    CWIST_HTTP_PARSE_TE_UNSUPPORTED,
    CWIST_HTTP_PARSE_EXPECT_FAILED
} cwist_http_parse_error_t;

/** --- Structures --- */

/// Linked list for headers to handle multiple headers easily
typedef struct cwist_http_header_node {
    cwist_sstring *key;
    cwist_sstring *value;
    struct cwist_http_header_node *next;
} cwist_http_header_node;

typedef struct cwist_http_request {
    cwist_http_method_t method;
    cwist_sstring *path;        ///< e.g., "/users/1"
    cwist_sstring *query;       ///< e.g., "active=true" (raw)
    cwist_query_map *query_params; ///< Parsed query parameters
    cwist_query_map *path_params;  ///< Parsed path parameters (e.g. :id)
    cwist_sstring *version;     ///< e.g., "HTTP/1.1"
    cwist_http_header_node *headers;
    cwist_sstring *body;
    bool keep_alive;
    int client_fd;
    bool upgraded;
    uint32_t stream_id;     ///< HTTP/2 or HTTP/3 stream ID (0 for HTTP/1.1).
    void *private_data;     ///< Internal framework use (protocol-specific context).
    size_t content_length;
} cwist_http_request;

typedef struct cwist_http_response {
    cwist_sstring *version;     ///< e.g., "HTTP/1.1"
    cwist_http_status_t status_code;
    cwist_sstring *status_text; ///< e.g., "OK"
    cwist_http_header_node *headers;
    cwist_sstring *body;
    bool keep_alive;
} cwist_http_response;

/** --- API Functions --- */

/** @name Request Lifecycle */
/** @{ */

/**
 * @brief Create a new HTTP request object.
 */
cwist_http_request *cwist_http_request_create(void);

/**
 * @brief Destroy an HTTP request object.
 */
void cwist_http_request_destroy(cwist_http_request *req);

/**
 * @brief Parse a raw HTTP request string.
 */
cwist_http_request *cwist_http_parse_request(const char *raw_request);

/**
 * @brief Receive and parse an HTTP request from a socket.
 */
cwist_http_request *cwist_http_receive_request(int client_fd, char *read_buf, size_t buf_size, size_t *buf_len, cwist_http_parse_error_t *err_out);

/**
 * @brief Send a minimal error response (Connection: close) for a failed request.
 */
void cwist_http_send_error_response(int fd, int status, const char *msg);

/** @} */

/** @name Response Lifecycle */
/** @{ */

/**
 * @brief Create a new HTTP response object.
 */
cwist_http_response *cwist_http_response_create(void);

/**
 * @brief Destroy an HTTP response object.
 */
void cwist_http_response_destroy(cwist_http_response *res);

/**
 * @brief Serialize a response to a string.
 */
cwist_sstring *cwist_http_stringify_response(cwist_http_response *res);

/**
 * @brief Send a response over a socket.
 */
cwist_error_t cwist_http_send_response(int client_fd, cwist_http_response *res);

/**
 * @brief Send only the status line and headers (HEAD replies).
 */
cwist_error_t cwist_http_send_response_head(int client_fd, cwist_http_response *res);

/**
 * @brief Serialize only the status line and headers into a caller buffer.
 * @return Number of bytes written.
 */
size_t cwist_http_serialize_headers(cwist_http_response *res, char *buf, size_t buf_size);

/** @} */

/** @name Header Manipulation */
/** @{ */

/**
 * @brief Add a header to the list.
 */
cwist_error_t cwist_http_header_add(cwist_http_header_node **head, const char *key, const char *value);

/**
 * @brief Find a header value by key.
 * @return Raw char* for convenience, NULL if not found.
 */
char *cwist_http_header_get(cwist_http_header_node *head, const char *key);

/**
 * @brief Add default security headers (CSP, X-Frame-Options, etc.) if missing.
 */
void cwist_http_response_add_security_headers(cwist_http_response *res);

/**
 * @brief Free all headers.
 */
void cwist_http_header_free_all(cwist_http_header_node *head);

/** @} */

/** @name Helpers */
/** @{ */

/**
 * @brief Convert method enum to string.
 */
const char *cwist_http_method_to_string(cwist_http_method_t method);

/**
 * @brief Convert method string to enum.
 */
cwist_http_method_t cwist_http_string_to_method(const char *method_str);

/** @} */

/** @name TCP Socket Helpers */
/** @{ */

/**
 * @brief Create an IPv4 socket and bind/listen.
 */
int cwist_make_socket_ipv4(struct sockaddr_in *sockv4, const char *address, uint16_t port, uint16_t backlog);

/**
 * @brief Accept sockets and invoke a handler callback.
 */
cwist_error_t cwist_accept_socket(int server_fd, struct sockaddr *sockv4, void (*handler_func)(int client_fd, void *ctx), void *ctx);

typedef struct cwist_server_config {
    bool use_forking;     ///< Process per request
    bool use_threading;   ///< Thread per request
    bool use_epoll;       ///< Use epoll for accepting
} cwist_server_config;

cwist_error_t cwist_http_server_loop(int server_fd, cwist_server_config *config, void (*handler)(int, void *), void *ctx);
int headers_have_content_length(cwist_http_header_node *headers);

extern const int CWIST_CREATE_SOCKET_FAILED;
extern const int CWIST_HTTP_UNAVAILABLE_ADDRESS;
extern const int CWIST_HTTP_BIND_FAILED;
extern const int CWIST_HTTP_SETSOCKOPT_FAILED;
extern const int CWIST_HTTP_LISTEN_FAILED;

#endif
