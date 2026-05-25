#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/http.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/sys/err/cwist_err.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/sys/app/shutdown.h>
#include <ttak/mols_control.h>
#include <ttak/net/lattice.h>
#include <ttak/priority/scheduler.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <strings.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>

#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef __linux__
#include <sys/epoll.h>
#include <sys/sendfile.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/event.h>
#endif

/* --- Thread Pool for HTTP --- */
#define HTTP_THREAD_POOL_SIZE 1024
#define HTTP_TASKS_PER_THREAD 32768

typedef struct {
    int client_fd;
    void (*handler_func)(int, void *);
    void *ctx;
} http_pool_task_t;

typedef struct {
    pthread_t thread;
    http_pool_task_t *queue;
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t cond_not_empty;
    pthread_cond_t cond_not_full;
    int shutdown;
    uint32_t worker_id;
} http_thread_worker_t;

static http_thread_worker_t g_workers[HTTP_THREAD_POOL_SIZE];
static size_t g_rr_index = 0;

static void *http_pool_worker(void *arg) {
    http_thread_worker_t *w = (http_thread_worker_t *)arg;
    /* Bind this thread to the lattice worker ID for deterministic slot selection. */
    ttak_net_lattice_set_worker_id(w->worker_id);

    while (1) {
        pthread_mutex_lock(&w->mutex);
        while (w->count == 0 && !w->shutdown) {
            pthread_cond_wait(&w->cond_not_empty, &w->mutex);
        }
        if (w->shutdown && w->count == 0) {
            pthread_mutex_unlock(&w->mutex);
            break;
        }
        http_pool_task_t task = w->queue[w->head];
        w->head = (w->head + 1) % HTTP_TASKS_PER_THREAD;
        w->count--;
        pthread_cond_signal(&w->cond_not_full);
        pthread_mutex_unlock(&w->mutex);

        task.handler_func(task.client_fd, task.ctx);
    }
    return NULL;
}

static int http_pool_init(void) {
    g_rr_index = 0;
    memset(g_workers, 0, sizeof(g_workers));
    for (int i = 0; i < HTTP_THREAD_POOL_SIZE; i++) {
        g_workers[i].queue = cwist_alloc(HTTP_TASKS_PER_THREAD * sizeof(http_pool_task_t));
        if (!g_workers[i].queue) return -1;
        
        pthread_mutex_init(&g_workers[i].mutex, NULL);
        pthread_cond_init(&g_workers[i].cond_not_empty, NULL);
        pthread_cond_init(&g_workers[i].cond_not_full, NULL);
        g_workers[i].worker_id = (uint32_t)i;
        if (pthread_create(&g_workers[i].thread, NULL, http_pool_worker, &g_workers[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static void http_pool_submit(int client_fd, void (*handler)(int, void *), void *ctx) {
    /* Deterministic worker selection using Choi Seok-jeong's MOLS to minimize cache bouncing. */
    uint16_t node_id = (uint16_t)(client_fd % TTAK_MOLS_NODE_COUNT);
    uint32_t mixed = ttak_apply_mols_control(node_id, (uint32_t)g_rr_index);
    size_t worker_idx = mixed % HTTP_THREAD_POOL_SIZE;

    g_rr_index = (g_rr_index + 1) % HTTP_THREAD_POOL_SIZE;
    
    http_thread_worker_t *w = &g_workers[worker_idx];

    pthread_mutex_lock(&w->mutex);
    while (w->count >= HTTP_TASKS_PER_THREAD && !w->shutdown) {
        pthread_cond_wait(&w->cond_not_full, &w->mutex);
    }
    if (w->shutdown) {
        pthread_mutex_unlock(&w->mutex);
        close(client_fd);
        return;
    }
    w->queue[w->tail].client_fd = client_fd;
    w->queue[w->tail].handler_func = handler;
    w->queue[w->tail].ctx = ctx;
    w->tail = (w->tail + 1) % HTTP_TASKS_PER_THREAD;
    w->count++;
    pthread_cond_signal(&w->cond_not_empty);
    pthread_mutex_unlock(&w->mutex);
}
static void http_pool_destroy(void) {
    for (int i = 0; i < HTTP_THREAD_POOL_SIZE; i++) {
        pthread_mutex_lock(&g_workers[i].mutex);
        g_workers[i].shutdown = 1;
        pthread_cond_broadcast(&g_workers[i].cond_not_empty);
        pthread_mutex_unlock(&g_workers[i].mutex);
    }
    for (int i = 0; i < HTTP_THREAD_POOL_SIZE; i++) {
        pthread_join(g_workers[i].thread, NULL);
        pthread_mutex_destroy(&g_workers[i].mutex);
        pthread_cond_destroy(&g_workers[i].cond_not_empty);
        pthread_cond_destroy(&g_workers[i].cond_not_full);
        if (g_workers[i].queue) {
            cwist_free(g_workers[i].queue);
        }
    }
}
/* --- End Thread Pool --- */

/**
 * @file http.c
 * @brief Core HTTP request/response allocation, serialization, socket, and server-loop helpers.
 */

const int CWIST_CREATE_SOCKET_FAILED     = -1;
const int CWIST_HTTP_UNAVAILABLE_ADDRESS = -2;
const int CWIST_HTTP_BIND_FAILED         = -3;
const int CWIST_HTTP_SETSOCKOPT_FAILED   = -4;
const int CWIST_HTTP_LISTEN_FAILED       = -5;

/* --- Helpers --- */

/**
 * @brief Convert a HTTP method enum into its wire-format token.
 * @param method HTTP method enum value.
 * @return Static string name for the method.
 */
const char *cwist_http_method_to_string(cwist_http_method_t method) {
    switch (method) {
        case CWIST_HTTP_GET: return "GET";
        case CWIST_HTTP_POST: return "POST";
        case CWIST_HTTP_PUT: return "PUT";
        case CWIST_HTTP_DELETE: return "DELETE";
        case CWIST_HTTP_PATCH: return "PATCH";
        case CWIST_HTTP_HEAD: return "HEAD";
        case CWIST_HTTP_OPTIONS: return "OPTIONS";
        default: return "UNKNOWN";
    }
}

/**
 * @brief Parse a method token into CWIST's HTTP method enum.
 * @param method_str Raw method token from the request line.
 * @return Parsed enum value, or CWIST_HTTP_UNKNOWN when unsupported.
 */
cwist_http_method_t cwist_http_string_to_method_len(const char *str, size_t len) {
    if (len == 3 && strncmp(str, "GET", 3) == 0) return CWIST_HTTP_GET;
    if (len == 4 && strncmp(str, "POST", 4) == 0) return CWIST_HTTP_POST;
    if (len == 3 && strncmp(str, "PUT", 3) == 0) return CWIST_HTTP_PUT;
    if (len == 6 && strncmp(str, "DELETE", 6) == 0) return CWIST_HTTP_DELETE;
    if (len == 5 && strncmp(str, "PATCH", 5) == 0) return CWIST_HTTP_PATCH;
    if (len == 4 && strncmp(str, "HEAD", 4) == 0) return CWIST_HTTP_HEAD;
    if (len == 7 && strncmp(str, "OPTIONS", 7) == 0) return CWIST_HTTP_OPTIONS;
    return CWIST_HTTP_UNKNOWN;
}

cwist_http_method_t cwist_http_string_to_method(const char *method_str) {
    if (!method_str) return CWIST_HTTP_UNKNOWN;
    return cwist_http_string_to_method_len(method_str, strlen(method_str));
}

/* --- Header Manipulation --- */

/**
 * @brief Prepend one header node to the linked-list header collection.
 * @param head Header-list head pointer to update.
 * @param key Header name to store.
 * @param value Header value to store.
 * @return Tagged CWIST error describing success or allocation failure.
 */
cwist_error_t cwist_http_header_add(cwist_http_header_node **head, const char *key, const char *value) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    
    cwist_http_header_node *node = (cwist_http_header_node *)cwist_alloc(sizeof(cwist_http_header_node));
    if (!node) {
        err = make_error(CWIST_ERR_JSON);
        err.error.err_json = cJSON_CreateObject();
        cJSON_AddStringToObject(err.error.err_json, "http_error", "Failed to allocate header");
        return err;
    }

    node->key = cwist_sstring_create();
    node->value = cwist_sstring_create();
    node->next = NULL;

    cwist_sstring_assign(node->key, (char *)key);
    cwist_sstring_assign(node->value, (char *)value);

    node->next = *head;
    *head = node;

    err.error.err_i16 = 0; // Success
    return err;
}

/**
 * @brief Find a header value using case-insensitive header-name comparison.
 * @param head Head of the header linked list.
 * @param key Header name to search for.
 * @return Raw header value string, or NULL when absent.
 */
char *cwist_http_header_get(cwist_http_header_node *head, const char *key) {
    cwist_http_header_node *curr = head;
    while (curr) {
        if (curr->key->data && strcasecmp(curr->key->data, key) == 0) {
            return curr->value->data;
        }
        curr = curr->next;
    }
    return NULL;
}

/**
 * @brief Add default security headers to an HTTP response if not already present.
 * @param res Response object to populate.
 */
void cwist_http_response_add_security_headers(cwist_http_response *res) {
    if (!res) return;

    if (!cwist_http_header_get(res->headers, "X-Frame-Options")) {
        cwist_http_header_add(&res->headers, "X-Frame-Options", "DENY");
    }
    if (!cwist_http_header_get(res->headers, "X-Content-Type-Options")) {
        cwist_http_header_add(&res->headers, "X-Content-Type-Options", "nosniff");
    }
    if (!cwist_http_header_get(res->headers, "Referrer-Policy")) {
        cwist_http_header_add(&res->headers, "Referrer-Policy", "strict-origin-when-cross-origin");
    }
    if (!cwist_http_header_get(res->headers, "Content-Security-Policy")) {
        cwist_http_header_add(&res->headers, "Content-Security-Policy",
            "default-src 'self'; "
            "script-src 'self' 'unsafe-inline' https://cdnjs.cloudflare.com https://cdn.jsdelivr.net; "
            "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com https://cdn.jsdelivr.net https://cdnjs.cloudflare.com; "
            "font-src 'self' https://fonts.gstatic.com; "
            "img-src 'self' data:; "
            "connect-src 'self'; "
            "frame-ancestors 'none'; "
            "base-uri 'self'; "
            "form-action 'self';");
    }
}

/**
 * @brief Destroy every node in a request or response header list.
 * @param head Head of the header linked list.
 */
void cwist_http_header_free_all(cwist_http_header_node *head) {
    cwist_http_header_node *curr = head;
    while (curr) {
        cwist_http_header_node *next = curr->next;
        cwist_sstring_destroy(curr->key);
        cwist_sstring_destroy(curr->value);
        cwist_free(curr);
        curr = next;
    }
}

/**
 * @brief Check whether a header key names the Connection header.
 * @param key Header key to inspect.
 * @return true when the key is "connection" ignoring case.
 */
static bool header_key_is_connection(const char *key) {
    if (!key) return false;
    return strcasecmp(key, "connection") == 0;
}

/**
 * @brief Check whether a Connection header requests socket closure.
 * @param value Header value to inspect.
 * @return true when the value equals "close" ignoring case.
 */
static bool header_value_is_close(const char *value) {
    if (!value) return false;
    return strcasecmp(value, "close") == 0;
}

/**
 * @brief Check whether a Connection header requests persistent keep-alive.
 * @param value Header value to inspect.
 * @return true when the value equals "keep-alive" ignoring case.
 */
static bool header_value_is_keep_alive(const char *value) {
    if (!value) return false;
    return strcasecmp(value, "keep-alive") == 0;
}

/**
 * @brief Detect whether the current header list already contains a Connection header.
 * @param head Head of the header linked list.
 * @return true when a Connection header is present.
 */
static bool headers_have_connection(cwist_http_header_node *head) {
    cwist_http_header_node *curr = head;
    while (curr) {
        if (curr->key && curr->key->data && header_key_is_connection(curr->key->data)) {
            return true;
        }
        curr = curr->next;
    }
    return false;
}

/* --- Request Lifecycle --- */

/**
 * @brief Allocate and initialize a default HTTP request object.
 * @return Newly allocated request, or NULL on allocation failure.
 */
cwist_http_request *cwist_http_request_create(void) {
    cwist_http_request *req = (cwist_http_request *)cwist_alloc(sizeof(cwist_http_request));
    if (!req) return NULL;

    req->method = CWIST_HTTP_GET; // Default
    req->path = cwist_sstring_create();
    req->query = cwist_sstring_create();
    req->query_params = cwist_query_map_create();
    req->path_params = cwist_query_map_create();
    req->version = cwist_sstring_create();
    req->headers = NULL;
    req->body = cwist_sstring_create();
    req->keep_alive = true;
    req->client_fd = -1;
    req->app = NULL;
    req->db = NULL;
    req->flash = cwist_query_map_create();
    req->upgraded = false;
    req->content_length = 0;
    req->stream_id = 0;
    req->private_data = NULL;
    req->endpoint_opts = CWIST_ENDPOINT_DEFAULT;

    // Defaults
    cwist_sstring_assign(req->version, "HTTP/1.1");
    cwist_sstring_assign(req->path, "/");

    return req;
}

/* --- Request Data Processing */

/**
 * @brief Resolve the peer IP address for a connected client socket.
 * @param fd Connected client socket descriptor.
 * @return Heap-allocated string containing the textual IP address.
 */
/**
 * @brief Format a time_t as an HTTP-date (RFC 7231).
 * @param t Unix timestamp.
 * @param buf Output buffer.
 * @param len Buffer capacity.
 */
void cwist_http_format_date(time_t t, char *buf, size_t len) {
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, len, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

/**
 * @brief Parse an HTTP-date string into a time_t.
 * @param str HTTP-date string.
 * @return Parsed timestamp, or (time_t)-1 on failure.
 */
time_t cwist_http_parse_date(const char *str) {
    if (!str) return (time_t)-1;
    struct tm tm = {0};
    const char *fmt = "%a, %d %b %Y %H:%M:%S %Z";
    if (strptime(str, fmt, &tm) == NULL) {
        // Try alternative formats
        fmt = "%a, %d-%b-%y %H:%M:%S %Z";
        if (strptime(str, fmt, &tm) == NULL) {
            fmt = "%a %b %d %H:%M:%S %Y";
            if (strptime(str, fmt, &tm) == NULL) {
                return (time_t)-1;
            }
        }
    }
    return timegm(&tm);
}

cwist_sstring* cwist_get_client_ip_from_fd(int fd) {
    cwist_sstring *s = cwist_sstring_create();
    cwist_sstring_assign(s, "127.0.0.1");
    // First, check if fd is available
    // If unavailable, return localhost
    if(fd <= 0) return s;

    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);

    // get client info
    if(getpeername(fd, (struct sockaddr *)&addr, &len) == -1) {
        fprintf(stdout, "[ERROR] Failed to get client info from file descriptor");
        return s;
    }

    char ip[INET6_ADDRSTRLEN];

    if(addr.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&addr;
        inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip));
    } else if(addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
        inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof(ip));
    }

    // assign found ip as a value
    cwist_sstring_assign(s, ip);
    return s;
}

/**
 * @brief Destroy a parsed HTTP request and all nested allocations it owns.
 * @param req Request object to destroy.
 */
void cwist_http_request_destroy(cwist_http_request *req) {
    if (req) {
        cwist_sstring_destroy(req->path);
        cwist_sstring_destroy(req->query);
        cwist_query_map_destroy(req->query_params);
        cwist_query_map_destroy(req->path_params);
        cwist_sstring_destroy(req->version);
        cwist_sstring_destroy(req->body);
        cwist_query_map_destroy(req->flash);
        cwist_http_header_free_all(req->headers);
        cwist_free(req);
    }
}

/* --- Response Lifecycle --- */

/**
 * @brief Release any file-stream state attached to a response.
 * @param res Response object whose streaming fields should be reset.
 */
static void cwist_http_response_release_file_stream(cwist_http_response *res) {
    if (!res || !res->use_file_stream) return;
    if (res->file_stream_auto_close && res->file_stream_fd >= 0) {
        close(res->file_stream_fd);
    }
    res->use_file_stream = false;
    res->file_stream_fd = -1;
    res->file_stream_len = 0;
    res->file_stream_offset = 0;
    res->file_stream_auto_close = false;
}

/**
 * @brief Release any zero-copy pointer-body cleanup hook attached to a response.
 * @param res Response object whose pointer-body state should be reset.
 */
static void cwist_http_response_release_ptr_body(cwist_http_response *res) {
    if (!res || !res->is_ptr_body) return;
    if (res->ptr_body_cleanup && res->ptr_body) {
        res->ptr_body_cleanup(res->ptr_body, res->ptr_body_len, res->ptr_body_cleanup_ctx);
    }
    res->is_ptr_body = false;
    res->ptr_body = NULL;
    res->ptr_body_len = 0;
    res->ptr_body_cleanup = NULL;
    res->ptr_body_cleanup_ctx = NULL;
}

/**
 * @brief Allocate and initialize a default HTTP response object.
 * @return Newly allocated response, or NULL on allocation failure.
 */
cwist_http_response *cwist_http_response_create(void) {
    cwist_http_response *res = (cwist_http_response *)cwist_alloc(sizeof(cwist_http_response));
    if (!res) return NULL;

    res->version = cwist_sstring_create();
    res->status_code = CWIST_HTTP_OK;
    res->status_text = cwist_sstring_create();
    res->headers = NULL;
    res->body = cwist_sstring_create();
    res->endpoint_opts = CWIST_ENDPOINT_DEFAULT;
    res->keep_alive = true;
    res->is_ptr_body = false;
    res->ptr_body = NULL;
    res->ptr_body_len = 0;
    res->ptr_body_cleanup = NULL;
    res->ptr_body_cleanup_ctx = NULL;
    res->use_file_stream = false;
    res->file_stream_fd = -1;
    res->file_stream_len = 0;
    res->file_stream_offset = 0;
    res->file_stream_auto_close = false;

    // Defaults
    cwist_sstring_assign(res->version, "HTTP/1.1");
    cwist_sstring_assign(res->status_text, "OK");

    cwist_http_response_add_security_headers(res);

    return res;
}

/**
 * @brief Destroy an HTTP response and release any attached body resources.
 * @param res Response object to destroy.
 */
void cwist_http_response_destroy(cwist_http_response *res) {
    if (res) {
        cwist_http_response_release_ptr_body(res);
        cwist_http_response_release_file_stream(res);
        cwist_sstring_destroy(res->version);
        cwist_sstring_destroy(res->status_text);
        cwist_sstring_destroy(res->body);
        cwist_http_header_free_all(res->headers);
        cwist_free(res->alt_svc);
        cwist_free(res);
    }
}

/**
 * @brief Attach an unmanaged zero-copy body pointer to a response.
 * @param res Response object to modify.
 * @param ptr External body pointer.
 * @param len Length of the external body in bytes.
 */
void cwist_http_response_set_body_ptr(cwist_http_response *res, const void *ptr, size_t len) {
    cwist_http_response_set_body_ptr_managed(res, ptr, len, NULL, NULL);
}

/**
 * @brief Attach a managed zero-copy body pointer and optional cleanup hook to a response.
 * @param res Response object to modify.
 * @param ptr External body pointer.
 * @param len Length of the external body in bytes.
 * @param cleanup Optional cleanup callback for the body pointer.
 * @param ctx Opaque context forwarded to the cleanup callback.
 */
void cwist_http_response_set_body_ptr_managed(cwist_http_response *res, const void *ptr, size_t len, cwist_http_body_cleanup_fn cleanup, void *ctx) {
    if (!res) return;
    cwist_http_response_release_file_stream(res);
    cwist_http_response_release_ptr_body(res);
    res->is_ptr_body = true;
    res->ptr_body = ptr;
    res->ptr_body_len = len;
    res->ptr_body_cleanup = cleanup;
    res->ptr_body_cleanup_ctx = ctx;
}

/**
 * @brief Set the Alt-Svc header value for HTTP/3 upgrade advertisement.
 * @param res Response object to modify.
 * @param alt_svc Alt-Svc header value (e.g., `h3=":443"; ma=86400`).
 *        Pass NULL to clear any previously set value.
 */
void cwist_http_response_set_alt_svc(cwist_http_response *res, const char *alt_svc) {
    if (!res) return;
    if (res->alt_svc) {
        cwist_free(res->alt_svc);
        res->alt_svc = NULL;
    }
    if (alt_svc) {
        res->alt_svc = strdup(alt_svc);
    }
}

// ... (request parsing omitted) ...

/**
 * @brief Detect whether a header list already defines Content-Length.
 * @param headers Header linked list to scan.
 * @return 1 when a Content-Length header is present, otherwise 0.
 */
int headers_have_content_length(cwist_http_header_node *headers) {
    cwist_http_header_node *curr = headers;
    while (curr) {
        if (curr->key && curr->key->data && strcasecmp(curr->key->data, "Content-Length") == 0) {
            return 1;
        }
        curr = curr->next;
    }
    return 0;
}

/**
 * @brief Serialize the HTTP status line and headers into a caller-provided buffer.
 * @param res Response object to serialize.
 * @param buf Destination buffer for the header block.
 * @param buf_size Total capacity of @p buf in bytes.
 * @return Number of bytes written into the buffer.
 */
static size_t serialize_headers(cwist_http_response *res, char *buf, size_t buf_size) {
    size_t body_len = 0;
    if (res->use_file_stream) {
        body_len = res->file_stream_len;
    } else if (res->is_ptr_body) {
        body_len = res->ptr_body_len;
    } else if (res->body) {
        body_len = res->body->size;
    }
    int offset = 0;
    
    // Status Line
    offset += snprintf(buf + offset, buf_size - offset, "%s %d %s\r\n",
             res->version->data ? res->version->data : "HTTP/1.1",
             res->status_code,
             res->status_text->data ? res->status_text->data : "OK");

    // Headers
    cwist_http_header_node *curr = res->headers;
    while (curr) {
        if (curr->key->data && curr->value->data) {
             offset += snprintf(buf + offset, buf_size - offset, "%s: %s\r\n", curr->key->data, curr->value->data);
        }
        curr = curr->next;
    }

    if (!headers_have_content_length(res->headers)) {
        offset += snprintf(buf + offset, buf_size - offset, "Content-Length: %zu\r\n", body_len);
    }

    if (!headers_have_connection(res->headers)) {
        if (res->keep_alive) {
            offset += snprintf(buf + offset, buf_size - offset, "Connection: keep-alive\r\n");
        } else {
            offset += snprintf(buf + offset, buf_size - offset, "Connection: close\r\n");
        }
    }

    if (res->alt_svc) {
        offset += snprintf(buf + offset, buf_size - offset, "Alt-Svc: %s\r\n", res->alt_svc);
    }

    offset += snprintf(buf + offset, buf_size - offset, "\r\n");
    return offset;
}

#include <sys/uio.h> // For writev and BSD sendfile

/**
 * @brief Attempt an optimized file-stream send path using platform sendfile support.
 * @param client_fd Connected client socket descriptor.
 * @param res Response object configured for file streaming.
 * @return true when the file body and headers were transmitted successfully.
 */
static bool cwist_http_stream_file_fast(int client_fd, cwist_http_response *res) {
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    if (!res || !res->use_file_stream || res->file_stream_fd < 0) return false;
    size_t remaining = res->file_stream_len;
    off_t offset = res->file_stream_offset;
    while (remaining > 0) {
#if defined(__linux__)
        ssize_t sent = sendfile(client_fd, res->file_stream_fd, &offset, remaining);
        if (sent < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return false;
        }
        if (sent == 0) break;
        remaining -= (size_t)sent;
#elif defined(__APPLE__)
        off_t chunk = (off_t)remaining;
        int rc = sendfile(res->file_stream_fd, client_fd, offset, &chunk, NULL, 0);
        if (rc == -1) {
            if (errno == EINTR || errno == EAGAIN) {
                if (chunk == 0) continue;
                offset += chunk;
                remaining -= (size_t)chunk;
                continue;
            }
            return false;
        }
        if (chunk == 0) break;
        offset += chunk;
        remaining -= (size_t)chunk;
#elif defined(__FreeBSD__)
        off_t sent = 0;
        size_t chunk = remaining;
        int rc = sendfile(res->file_stream_fd, client_fd, offset, chunk, NULL, &sent, 0);
        if (rc == -1) {
            if (errno == EINTR || errno == EAGAIN) {
                if (sent == 0) continue;
                offset += sent;
                remaining -= (size_t)sent;
                continue;
            }
            return false;
        }
        if (sent == 0) break;
        offset += sent;
        remaining -= (size_t)sent;
#endif
    }
    res->file_stream_offset = offset;
    return remaining == 0;
#else
    (void)client_fd;
    (void)res;
    errno = ENOTSUP;
    return false;
#endif
}

/**
 * @brief Serialize and send an HTTP response to a connected client socket.
 * @param client_fd Connected client socket descriptor.
 * @param res Response object to send.
 * @return Tagged CWIST error describing success or transmission failure.
 */
cwist_error_t cwist_http_send_response(int client_fd, cwist_http_response *res) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (client_fd < 0 || !res) {
        err.error.err_i16 = -1;
        return err;
    }

    // 1. Prepare Headers (On Stack)
    char header_buf[CWIST_HTTP_MAX_HEADER_SIZE];
    size_t header_len = serialize_headers(res, header_buf, sizeof(header_buf));

    // 2. Prepare Body
    const void *body_ptr = NULL;
    size_t body_len = 0;

    if (res->is_ptr_body) {
        body_ptr = res->ptr_body;
        body_len = res->ptr_body_len;
    } else if (res->body && res->body->data) {
        body_ptr = res->body->data;
        body_len = res->body->size;
    }

    // 3. sendmsg (Scatter/Gather + Flags) - Zero Copy Send
    struct msghdr msg = {0};
    struct iovec iov[2];
    int iov_cnt = 1;

    iov[0].iov_base = header_buf;
    iov[0].iov_len = header_len;

    if (!res->use_file_stream && body_len > 0 && body_ptr) {
        iov[1].iov_base = (void*)body_ptr;
        iov[1].iov_len = body_len;
        iov_cnt = 2;
    }

    msg.msg_iov = iov;
    msg.msg_iovlen = iov_cnt;

    int flags = 0;
    #if defined(MSG_NOSIGNAL)
    flags = MSG_NOSIGNAL;
    #endif

    ssize_t written = sendmsg(client_fd, &msg, flags);
    if (written < 0) {
        err.error.err_i16 = -1;
    } else {
        err.error.err_i16 = 0;
        if (res->use_file_stream) {
            if (!cwist_http_stream_file_fast(client_fd, res)) {
                err.error.err_i16 = -1;
            }
        }
    }

    cwist_http_response_release_ptr_body(res);
    cwist_http_response_release_file_stream(res);
    return err;
}

/**
 * @brief Materialize an HTTP response into a contiguous string for debugging or TLS writes.
 * @param res Response object to stringify.
 * @return Heap-allocated response string, or NULL on invalid input.
 */
cwist_sstring *cwist_http_stringify_response(cwist_http_response *res) {
    // Deprecated / Debug only
    if (!res) return NULL;
    cwist_sstring *s = cwist_sstring_create();
    char header_buf[CWIST_HTTP_MAX_HEADER_SIZE];
    serialize_headers(res, header_buf, sizeof(header_buf));
    cwist_sstring_assign(s, header_buf);
    if (res->is_ptr_body && res->ptr_body) {
        cwist_sstring_append_len(s, (char*)res->ptr_body, res->ptr_body_len);
    } else if (res->body) {
        cwist_sstring_append(s, res->body->data);
    }
    return s;
}

/**
 * @brief Parse a raw HTTP request buffer into a CWIST request object.
 * @param raw_request NUL-terminated request buffer containing headers and optional body.
 * @return Parsed request object, or NULL on malformed input.
 */
cwist_http_request *cwist_http_parse_request(const char *raw_request) {
    if (!raw_request) return NULL;

    cwist_http_request *req = cwist_http_request_create();
    if (!req) return NULL;
    
    const char *line_start = raw_request;
    const char *header_end = strstr(raw_request, "\r\n\r\n");
    if (!header_end) {
        cwist_http_request_destroy(req);
        return NULL;
    }

    const char *line_end = strstr(line_start, "\r\n");
    if (!line_end || line_end > header_end) { 
        cwist_http_request_destroy(req); 
        return NULL; 
    }

    // 1. Request Line (Optimized: No intermediate allocation)
    const char *sp1 = strchr(line_start, ' ');
    if (!sp1 || sp1 > line_end) { cwist_http_request_destroy(req); return NULL; }
    const char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2 || sp2 > line_end) { cwist_http_request_destroy(req); return NULL; }

    req->method = cwist_http_string_to_method_len(line_start, sp1 - line_start);
    
    const char *path_start = sp1 + 1;
    const char *path_end = sp2;
    const char *query_sep = memchr(path_start, '?', path_end - path_start);
    
    if (query_sep) {
        cwist_sstring_assign_len(req->path, path_start, query_sep - path_start);
        cwist_sstring_assign_len(req->query, query_sep + 1, path_end - (query_sep + 1));
        cwist_query_map_parse(req->query_params, req->query->data);
    } else {
        cwist_sstring_assign_len(req->path, path_start, path_end - path_start);
        cwist_sstring_assign_len(req->query, "", 0);
    }

    cwist_sstring_assign_len(req->version, sp2 + 1, line_end - (sp2 + 1));
    if (strncmp(sp2 + 1, "HTTP/1.1", 8) == 0) {
        req->keep_alive = true;
    } else {
        req->keep_alive = false;
    }

    // 2. Headers (Optimized: Minimal copies)
    line_start = line_end + 2; 
    while (line_start < header_end) {
        line_end = strstr(line_start, "\r\n");
        if (!line_end || line_end == line_start) break;

        const char *colon = memchr(line_start, ':', line_end - line_start);
        if (colon) {
            int key_len = colon - line_start;
            const char *val_start = colon + 1;
            while (val_start < line_end && *val_start == ' ') val_start++;
            int val_len = line_end - val_start;
            
            // Temporary NUL termination for legacy header_add
            char key_tmp[256];
            char val_tmp[1024];
            if (key_len < 256 && val_len < 1024) {
                memcpy(key_tmp, line_start, key_len); key_tmp[key_len] = '\0';
                memcpy(val_tmp, val_start, val_len); val_tmp[val_len] = '\0';
                
                cwist_http_header_add(&req->headers, key_tmp, val_tmp);
                
                if (strcasecmp(key_tmp, "Connection") == 0) {
                    if (strcasecmp(val_tmp, "close") == 0) req->keep_alive = false;
                    else if (strcasecmp(val_tmp, "keep-alive") == 0) req->keep_alive = true;
                } else if (strcasecmp(key_tmp, "Content-Length") == 0) {
                    req->content_length = (size_t)atoll(val_tmp);
                }
            }
        }
        line_start = line_end + 2;
    }

    const char *body_start = header_end + 4;
    if (*body_start != '\0') {
        cwist_sstring_assign(req->body, (char*)body_start);
    }

    return req;
}



/**
 * @brief Read from a client socket until a complete HTTP request is available.
 * @param client_fd Connected client socket descriptor.
 * @param read_buf Reusable receive buffer supplied by the caller.
 * @param buf_size Total capacity of @p read_buf.
 * @param buf_len In/out length of buffered leftover data.
 * @return Parsed request object, or NULL on timeout, parse failure, or IO failure.
 */
cwist_http_request *cwist_http_receive_request(int client_fd, char *read_buf, size_t buf_size, size_t *buf_len) {
    size_t total_received = *buf_len;
    char *header_end = NULL;

    // 1. Read until headers are complete
    while (!(header_end = strstr(read_buf, "\r\n\r\n"))) {
        if (total_received >= buf_size - 1) {
            // Buffer full, but headers not complete
            return NULL;
        }
        
        struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
        int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
        if (ret <= 0) return NULL; // Timeout or error

        ssize_t bytes = recv(client_fd, read_buf + total_received, buf_size - 1 - total_received, 0);
        if (bytes <= 0) return NULL;
        total_received += (size_t)bytes;
        read_buf[total_received] = '\0';
    }

    cwist_http_request *req = cwist_http_parse_request(read_buf);
    if (!req) return NULL;

    size_t header_len = (header_end + 4) - read_buf;
    size_t body_received = total_received - header_len;

    // 2. Read body based on Content-Length
    if (req->content_length > 0) {
        if (req->content_length > CWIST_HTTP_MAX_BODY_SIZE) {
            cwist_http_request_destroy(req);
            return NULL;
        }

        // Allocate body
        char *body = cwist_alloc(req->content_length + 1);
        if (!body) {
            cwist_http_request_destroy(req);
            return NULL;
        }

        size_t to_copy = (body_received < req->content_length) ? body_received : req->content_length;
        memcpy(body, header_end + 4, to_copy);
        size_t current_body_len = to_copy;

        while (current_body_len < req->content_length) {
            struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
            int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
            if (ret <= 0) {
                cwist_free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }

            ssize_t bytes = recv(client_fd, body + current_body_len, req->content_length - current_body_len, 0);
            if (bytes <= 0) {
                cwist_free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }
            current_body_len += (size_t)bytes;
        }
        body[req->content_length] = '\0';
        cwist_sstring_assign_len(req->body, body, req->content_length);
        cwist_free(body);

        // Calculate leftovers
        if (body_received > req->content_length) {
            size_t leftover_len = body_received - req->content_length;
            memmove(read_buf, header_end + 4 + req->content_length, leftover_len);
            *buf_len = leftover_len;
        } else {
            *buf_len = 0;
        }
    } else {
        // No body, leftovers are everything after headers
        if (body_received > 0) {
            memmove(read_buf, header_end + 4, body_received);
            *buf_len = body_received;
        } else {
            *buf_len = 0;
        }
    }
    read_buf[*buf_len] = '\0';

    return req;
}

typedef struct {
    const char *ext;
    const char *mime;
} cwist_mime_entry;

static const cwist_mime_entry CWIST_MIME_TABLE[] = {
    { ".html", "text/html; charset=utf-8" },
    { ".htm",  "text/html; charset=utf-8" },
    { ".css",  "text/css; charset=utf-8" },
    { ".js",   "application/javascript" },
    { ".mjs",  "application/javascript" },
    { ".json", "application/json" },
    { ".wasm", "application/wasm" },
    { ".png",  "image/png" },
    { ".jpg",  "image/jpeg" },
    { ".jpeg", "image/jpeg" },
    { ".gif",  "image/gif" },
    { ".svg",  "image/svg+xml" },
    { ".txt",  "text/plain; charset=utf-8" },
    { ".ico",  "image/x-icon" }
};

static const char *cwist_guess_mime(const char *file_path) {
    if (!file_path) return "application/octet-stream";
    const char *dot = strrchr(file_path, '.');
    if (!dot) {
        return "application/octet-stream";
    }
    for (size_t i = 0; i < sizeof(CWIST_MIME_TABLE) / sizeof(CWIST_MIME_TABLE[0]); i++) {
        if (strcasecmp(dot, CWIST_MIME_TABLE[i].ext) == 0) {
            return CWIST_MIME_TABLE[i].mime;
        }
    }
    return "application/octet-stream";
}

/**
 * @brief Prepare a response to serve a file either by buffering or direct streaming.
 * @param res Response object to populate.
 * @param file_path Filesystem path to the file that should be served.
 * @param content_type_hint Optional MIME type override.
 * @param out_size Optional output pointer receiving the file size in bytes.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_http_response_send_file(cwist_http_response *res, const char *file_path, const char *content_type_hint, size_t *out_size) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!res || !file_path) {
        err.error.err_i16 = -EINVAL;
        return err;
    }

    cwist_http_response_release_file_stream(res);
    cwist_http_response_release_ptr_body(res);

    int fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        err.error.err_i16 = -errno;
        return err;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        err.error.err_i16 = -errno;
        close(fd);
        return err;
    }

    if (!S_ISREG(st.st_mode)) {
        close(fd);
        err.error.err_i16 = -EISDIR;
        return err;
    }

    bool endpoint_file = cwist_endpoint_has(res->endpoint_opts, CWIST_ENDPOINT_FILE);

    if (!endpoint_file && (size_t)st.st_size > CWIST_HTTP_MAX_BODY_SIZE) {
        close(fd);
        err.error.err_i16 = -EFBIG;
        return err;
    }

    size_t file_size = (size_t)st.st_size;
    bool use_fast_stream = false;

    if (file_size > 0 && endpoint_file) {
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
        res->use_file_stream = true;
        res->file_stream_fd = fd;
        res->file_stream_len = file_size;
        res->file_stream_offset = 0;
        res->file_stream_auto_close = true;
        use_fast_stream = true;
#endif
    }

    char *buffer = NULL;

    if (!use_fast_stream && file_size > 0) {
        buffer = (char *)cwist_alloc(file_size);
        if (!buffer) {
            close(fd);
            err.error.err_i16 = -ENOMEM;
            return err;
        }
    }

    if (!use_fast_stream) {
        size_t total_read = 0;
        while (total_read < file_size) {
            ssize_t bytes = read(fd, buffer + total_read, file_size - total_read);
            if (bytes < 0) {
                if (errno == EINTR) continue;
                err.error.err_i16 = -errno;
                cwist_free(buffer);
                close(fd);
                return err;
            }
            if (bytes == 0) {
                err.error.err_i16 = -EIO;
                cwist_free(buffer);
                close(fd);
                return err;
            }
            total_read += (size_t)bytes;
        }
        close(fd);

        if (file_size > 0) {
            cwist_sstring_assign_len(res->body, buffer, file_size);
            cwist_free(buffer);
        } else {
            cwist_sstring_assign(res->body, "");
        }
    } else {
        cwist_sstring_assign(res->body, "");
    }

    const char *mime = content_type_hint ? content_type_hint : cwist_guess_mime(file_path);
    if (mime && !cwist_http_header_get(res->headers, "Content-Type")) {
        cwist_http_header_add(&res->headers, "Content-Type", mime);
    }

    if (out_size) {
        *out_size = file_size;
    }

    res->status_code = CWIST_HTTP_OK;
    err.error.err_i16 = 0;
    return err;
}

/* --- Predefined Static Blobs --- */

const char CWIST_BLOB_200_OK[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
const char CWIST_BLOB_404[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 13\r\nConnection: keep-alive\r\n\r\n404 Not Found";
const char CWIST_BLOB_500[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 21\r\nConnection: close\r\n\r\nInternal Server Error";

/* --- Socket Manipulation --- */

/**
 * @brief Create, configure, bind, and listen on an IPv4 TCP socket.
 * @param sockv4 Output sockaddr structure populated for the bind call.
 * @param address IPv4 address string to bind.
 * @param port TCP port to listen on.
 * @param backlog Listen backlog passed to listen(2).
 * @return Listening socket fd on success, or a negative CWIST socket error code.
 */
int cwist_make_socket_ipv4(struct sockaddr_in *sockv4, const char *address, uint16_t port, uint16_t backlog) {
  int server_fd = -1;
  int opt = 1;
  in_addr_t addr = inet_addr(address);

  if(addr == INADDR_NONE) {
    return CWIST_HTTP_UNAVAILABLE_ADDRESS;
  }

  if((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    cJSON *err_json = cJSON_CreateObject();
    cJSON_AddStringToObject(err_json, "err", "Failed to create IPv4 socket");
    char *cjson_error_log = cJSON_Print(err_json);
    perror(cjson_error_log);
    cwist_free(cjson_error_log);
    cJSON_Delete(err_json);

    return CWIST_CREATE_SOCKET_FAILED;
  }

  if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
    cJSON *err_json = cJSON_CreateObject();
    cJSON_AddStringToObject(err_json, "err", "Failed to set up IPv4 socket options");
    char *cjson_error_log = cJSON_Print(err_json);
    perror(cjson_error_log);
    cwist_free(cjson_error_log);
    cJSON_Delete(err_json);

    return CWIST_HTTP_SETSOCKOPT_FAILED;  
  }

#ifdef SO_REUSEPORT
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
  int no_sig_pipe = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sig_pipe, sizeof(no_sig_pipe));
#endif

  sockv4->sin_family = AF_INET;

  sockv4->sin_addr.s_addr = addr;
  sockv4->sin_port = htons(port);

  if(bind(server_fd, (struct sockaddr *)sockv4, sizeof(struct sockaddr_in)) < 0) {
    cJSON *err_json = cJSON_CreateObject();
    cJSON_AddStringToObject(err_json, "err", "Failed to bind IPv4 socket");
    char *cjson_error_log = cJSON_Print(err_json);
    perror(cjson_error_log);
    cwist_free(cjson_error_log);
    cJSON_Delete(err_json);

    return CWIST_HTTP_BIND_FAILED;
  }

  if(listen(server_fd, backlog) < 0) {
    cJSON *err_json = cJSON_CreateObject();
    char err_msg[128];
    char err_format[128] = "Failed to listen at %s:%d";
    snprintf(err_msg, 127, err_format, address, port);

    cJSON_AddStringToObject(err_json, "err", err_msg);
    char *cjson_error_log = cJSON_Print(err_json);
    perror(cjson_error_log);
    cwist_free(cjson_error_log);
    cJSON_Delete(err_json);

    return CWIST_HTTP_LISTEN_FAILED;
  }

  return server_fd;
}

/**
 * @brief Decide whether an accept(2) error should be treated as transient.
 * @param err errno value returned by accept(2).
 * @return true when the caller should retry the accept loop.
 */
static bool cwist_accept_error_should_retry(int err) {
    switch (err) {
        case EINTR:
        case EAGAIN:
        case ECONNABORTED:
#ifdef ECONNRESET
        case ECONNRESET:
#endif
#ifdef EPROTO
        case EPROTO:
#endif
            return true;
        case EMFILE:
        case ENFILE:
        case ENOBUFS:
        case ENOMEM:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Apply a small sleep when repeated accept failures suggest resource pressure.
 * @param err errno value returned by accept(2).
 */
static void cwist_accept_error_backoff(int err) {
    switch (err) {
        case EMFILE:
        case ENFILE:
        case ENOBUFS:
        case ENOMEM: {
            fprintf(stderr, "[CWIST] accept() backoff triggered: %s\n", strerror(err));
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 50 * 1000 * 1000; // 50ms
            nanosleep(&ts, NULL);
            break;
        }
        default:
            break;
    }
}

/**
 * @brief Service one accepted client in a forked child process.
 * @param client_fd Accepted client socket descriptor.
 * @param handler_func Request handler callback.
 * @param ctx Opaque callback context.
 */
static void handle_client_forking(int client_fd, void (*handler_func)(int, void *), void *ctx) {
    pid_t pid = fork();
    if (pid == 0) {
        handler_func(client_fd, ctx);
        close(client_fd);
        _exit(0);
    } else if (pid > 0) {
        close(client_fd);
    }
}

/**
 * @brief Accept one client connection and dispatch it according to the current server strategy.
 * @param server_fd Listening server socket.
 * @param sockv4 Scratch sockaddr buffer for accept(2).
 * @param handler_func Callback that handles one accepted client.
 * @param ctx Opaque callback context.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_accept_socket(int server_fd, struct sockaddr *sockv4, void (*handler_func)(int client_fd, void *), void *ctx) {
  int client_fd = -1;
  struct sockaddr_in peer_addr;
  socklen_t addrlen = sizeof(peer_addr);

  while(true) { 
    if((client_fd = accept(server_fd, (struct sockaddr *)&peer_addr, &addrlen)) < 0) {
      if (errno == EINTR) continue;
// ... (error handling)
      if (errno == EBADF || errno == EINVAL || errno == ENOTSOCK) {
          fprintf(stderr, "Fatal socket error %d. Exiting accept loop.\n", errno);
          break;
      }
      continue;
    }

    if (sockv4) {
      memcpy(sockv4, &peer_addr, sizeof(peer_addr));
    }

    handler_func(client_fd, ctx);
  }

  cwist_error_t err = make_error(CWIST_ERR_INT16);
  err.error.err_i16 = -1;
  return err;
}

/**
 * @brief Run the main HTTP accept loop using the configured concurrency strategy.
 * @param server_fd Listening server socket.
 * @param config Server concurrency configuration flags.
 * @param handler Callback that handles one accepted client.
 * @param ctx Opaque callback context.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_http_server_loop(int server_fd, cwist_server_config *config, void (*handler)(int, void *), void *ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!config || server_fd < 0 || !handler) {
        err.error.err_i16 = -1;
        return err;
    }

    if (config->use_forking) {
        while (atomic_load(&g_cwist_running)) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) {
                int accept_err = errno;
                if (accept_err == EBADF || accept_err == EINVAL) break;
                if (cwist_accept_error_should_retry(accept_err)) {
                    cwist_accept_error_backoff(accept_err);
                    continue;
                }
                err.error.err_i16 = -1;
                return err;
            }
            handle_client_forking(client_fd, handler, ctx);
        }
    }

    if (config->use_threading) {
        if (http_pool_init() != 0) {
            err.error.err_i16 = -1;
            return err;
        }
        while (atomic_load(&g_cwist_running)) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) {
                int accept_err = errno;
                if (accept_err == EBADF || accept_err == EINVAL) break;
                if (cwist_accept_error_should_retry(accept_err)) {
                    cwist_accept_error_backoff(accept_err);
                    continue;
                }
                err.error.err_i16 = -1;
                http_pool_destroy();
                return err;
            }
            http_pool_submit(client_fd, handler, ctx);
        }
        http_pool_destroy();
    }

#ifdef __linux__
    if (config->use_epoll) {
        int flags = fcntl(server_fd, F_GETFL, 0);
        if (flags >= 0) fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

        int epoll_fd = epoll_create1(0);
        if (epoll_fd < 0) {
            err.error.err_i16 = -1;
            return err;
        }
        struct epoll_event event;
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = server_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) < 0) {
            close(epoll_fd);
            err.error.err_i16 = -1;
            return err;
        }

        while (atomic_load(&g_cwist_running)) {
            struct epoll_event events[1024];
            int count = epoll_wait(epoll_fd, events, 1024, -1);
            if (count < 0) {
                if (errno == EINTR) continue;
                if (errno == EBADF) break;
                break;
            }
            for (int i = 0; i < count; i++) {
                if (events[i].data.fd == server_fd) {
                    while (1) {
                        int client_fd = accept(server_fd, NULL, NULL);
                        if (client_fd >= 0) {
                            handler(client_fd, ctx);
                        } else {
                            int accept_err = errno;
                            if (accept_err == EAGAIN || accept_err == EWOULDBLOCK) break;
                            if (accept_err == EBADF || accept_err == EINVAL) goto epoll_exit;
                            if (accept_err == EINTR) continue;
                            if (cwist_accept_error_should_retry(accept_err)) {
                                cwist_accept_error_backoff(accept_err);
                                continue;
                            }
                            err.error.err_i16 = -1;
                            close(epoll_fd);
                            return err;
                        }
                    }
                }
            }
        }
epoll_exit:
        close(epoll_fd);
    }
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    if (config->use_epoll) {
        int flags = fcntl(server_fd, F_GETFL, 0);
        if (flags >= 0) fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

        int kqueue_fd = kqueue();
        if (kqueue_fd < 0) {
            err.error.err_i16 = -1;
            return err;
        }
        struct kevent change;
        EV_SET(&change, server_fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
        if (kevent(kqueue_fd, &change, 1, NULL, 0, NULL) < 0) {
            close(kqueue_fd);
            err.error.err_i16 = -1;
            return err;
        }

        while (atomic_load(&g_cwist_running)) {
            struct kevent events[16];
            int count = kevent(kqueue_fd, NULL, 0, events, 16, NULL);
            if (count < 0) {
                if (errno == EINTR) continue;
                if (errno == EBADF) break;
                break;
            }
            for (int i = 0; i < count; i++) {
                if ((int)events[i].ident == server_fd) {
                    while (1) {
                        int client_fd = accept(server_fd, NULL, NULL);
                        if (client_fd >= 0) {
                            handler(client_fd, ctx);
                        } else {
                            int accept_err = errno;
                            if (accept_err == EAGAIN || accept_err == EWOULDBLOCK) break;
                            if (accept_err == EBADF || accept_err == EINVAL) goto kq_exit;
                            if (accept_err == EINTR) continue;
                            if (cwist_accept_error_should_retry(accept_err)) {
                                cwist_accept_error_backoff(accept_err);
                                continue;
                            }
                            err.error.err_i16 = -1;
                            close(kqueue_fd);
                            return err;
                        }
                    }
                }
            }
        }
kq_exit:
        close(kqueue_fd);
    }
#endif

    return cwist_accept_socket(server_fd, NULL, handler, ctx);
}
