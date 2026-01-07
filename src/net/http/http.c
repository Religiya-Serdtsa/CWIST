#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/http.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/sys/err/cwist_err.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <strings.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>

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
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/event.h>
#endif

const int CWIST_CREATE_SOCKET_FAILED     = -1;
const int CWIST_HTTP_UNAVAILABLE_ADDRESS = -2;
const int CWIST_HTTP_BIND_FAILED         = -3;
const int CWIST_HTTP_SETSOCKOPT_FAILED   = -4;
const int CWIST_HTTP_LISTEN_FAILED       = -5;

/* --- Helpers --- */

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

cwist_http_method_t cwist_http_string_to_method(const char *method_str) {
    if (strcmp(method_str, "GET") == 0) return CWIST_HTTP_GET;
    if (strcmp(method_str, "POST") == 0) return CWIST_HTTP_POST;
    if (strcmp(method_str, "PUT") == 0) return CWIST_HTTP_PUT;
    if (strcmp(method_str, "DELETE") == 0) return CWIST_HTTP_DELETE;
    if (strcmp(method_str, "PATCH") == 0) return CWIST_HTTP_PATCH;
    if (strcmp(method_str, "HEAD") == 0) return CWIST_HTTP_HEAD;
    if (strcmp(method_str, "OPTIONS") == 0) return CWIST_HTTP_OPTIONS;
    return CWIST_HTTP_UNKNOWN;
}

/* --- Header Manipulation --- */

cwist_error_t cwist_http_header_add(cwist_http_header_node **head, const char *key, const char *value) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    
    cwist_http_header_node *node = (cwist_http_header_node *)malloc(sizeof(cwist_http_header_node));
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

char *cwist_http_header_get(cwist_http_header_node *head, const char *key) {
    cwist_http_header_node *curr = head;
    while (curr) {
        // case-insensitive comparison for headers is standard, but keeping it strict for now for simplicity
        if (curr->key->data && strcmp(curr->key->data, key) == 0) {
            return curr->value->data;
        }
        curr = curr->next;
    }
    return NULL;
}

void cwist_http_header_free_all(cwist_http_header_node *head) {
    cwist_http_header_node *curr = head;
    while (curr) {
        cwist_http_header_node *next = curr->next;
        cwist_sstring_destroy(curr->key);
        cwist_sstring_destroy(curr->value);
        free(curr);
        curr = next;
    }
}

static bool header_key_is_connection(const char *key) {
    if (!key) return false;
    return strcasecmp(key, "connection") == 0;
}

static bool header_value_is_close(const char *value) {
    if (!value) return false;
    return strcasecmp(value, "close") == 0;
}

static bool header_value_is_keep_alive(const char *value) {
    if (!value) return false;
    return strcasecmp(value, "keep-alive") == 0;
}

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

cwist_http_request *cwist_http_request_create(void) {
    cwist_http_request *req = (cwist_http_request *)malloc(sizeof(cwist_http_request));
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
    req->upgraded = false;
    req->content_length = 0;

    // Defaults
    cwist_sstring_assign(req->version, "HTTP/1.1");
    cwist_sstring_assign(req->path, "/");

    return req;
}

/* --- Request Data Processing */

// Get client IP from client file descriptor                                                                           
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

void cwist_http_request_destroy(cwist_http_request *req) {
    if (req) {
        cwist_sstring_destroy(req->path);
        cwist_sstring_destroy(req->query);
        cwist_query_map_destroy(req->query_params);
        cwist_query_map_destroy(req->path_params);
        cwist_sstring_destroy(req->version);
        cwist_sstring_destroy(req->body);
        cwist_http_header_free_all(req->headers);
        free(req);
    }
}

/* --- Response Lifecycle --- */

cwist_http_response *cwist_http_response_create(void) {
    cwist_http_response *res = (cwist_http_response *)malloc(sizeof(cwist_http_response));
    if (!res) return NULL;

    res->version = cwist_sstring_create();
    res->status_code = CWIST_HTTP_OK;
    res->status_text = cwist_sstring_create();
    res->headers = NULL;
    res->body = cwist_sstring_create();
    res->keep_alive = true;

    // Defaults
    cwist_sstring_assign(res->version, "HTTP/1.1");
    cwist_sstring_assign(res->status_text, "OK");

    return res;
}

void cwist_http_response_destroy(cwist_http_response *res) {
    if (res) {
        cwist_sstring_destroy(res->version);
        cwist_sstring_destroy(res->status_text);
        cwist_sstring_destroy(res->body);
        cwist_http_header_free_all(res->headers);
        free(res);
    }
}

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

    // 1. Request Line
    int request_line_len = line_end - line_start;
    char *request_line = (char*)malloc(request_line_len + 1);
    if (!request_line) {
        cwist_http_request_destroy(req);
        return NULL;
    }
    strncpy(request_line, line_start, request_line_len);
    request_line[request_line_len] = '\0';
    
    char *next_ptr;
    char *method_str = strtok_r(request_line, " ", &next_ptr);
    char *path_str = strtok_r(NULL, " ", &next_ptr);
    char *version_str = strtok_r(NULL, " ", &next_ptr);
    
    if (method_str) req->method = cwist_http_string_to_method(method_str);
    if (path_str) {
      char *query = strchr(path_str, '?');
      if(query) {
        *query = '\0';
        cwist_sstring_assign(req->path, path_str);
        cwist_sstring_assign(req->query, query + 1); // exclude ? mark
        cwist_query_map_parse(req->query_params, req->query->data);
      } else {
        cwist_sstring_assign(req->path, path_str);
        cwist_sstring_assign(req->query, "");
      }
    }

    if (version_str) {
        cwist_sstring_assign(req->version, version_str);
        if (strcmp(version_str, "HTTP/1.1") == 0) {
            req->keep_alive = true;
        } else {
            req->keep_alive = false;
        }
    }
    
    free(request_line);

    // 2. Headers
    line_start = line_end + 2; // Skip \r\n
    while (line_start < header_end) {
        line_end = strstr(line_start, "\r\n");
        if (!line_end) break;

        if (line_end == line_start) {
            // Empty line found
            line_start += 2;
            break;
        }
        
        int header_len = line_end - line_start;
        char *header_line = (char*)malloc(header_len + 1);
        if (header_line) {
            strncpy(header_line, line_start, header_len);
            header_line[header_len] = '\0';
            
            char *colon = strchr(header_line, ':');
            if (colon) {
                *colon = '\0';
                char *key = header_line;
                char *value = colon + 1;
                while (*value == ' ') value++; // Trim leading space
                
                cwist_http_header_add(&req->headers, key, value);
                if (header_key_is_connection(key)) {
                    if (header_value_is_close(value)) {
                        req->keep_alive = false;
                    } else if (header_value_is_keep_alive(value)) {
                        req->keep_alive = true;
                    }
                } else if (strcasecmp(key, "Content-Length") == 0) {
                    req->content_length = (size_t)atoll(value);
                }
            }
            free(header_line);
        }
        
        line_start = line_end + 2;
    }

    const char *body_start = header_end + 4;
    if (*body_start != '\0') {
        cwist_sstring_assign(req->body, (char*)body_start);
    }

    return req;
}



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
        char *body = malloc(req->content_length + 1);
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
                free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }

            ssize_t bytes = recv(client_fd, body + current_body_len, req->content_length - current_body_len, 0);
            if (bytes <= 0) {
                free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }
            current_body_len += (size_t)bytes;
        }
        body[req->content_length] = '\0';
        cwist_sstring_assign_len(req->body, body, req->content_length);
        free(body);

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


cwist_sstring *cwist_http_stringify_response(cwist_http_response *res) {
    if (!res) return NULL;

    cwist_sstring *response_str = cwist_sstring_create();
    size_t body_len = 0;
    if (res->body) {
        body_len = res->body->size;
    }

    // Status Line
    char status_line[256];
    snprintf(status_line, sizeof(status_line), "%s %d %s\r\n",
             res->version->data ? res->version->data : "HTTP/1.1",
             res->status_code,
             res->status_text->data ? res->status_text->data : "OK");
    cwist_sstring_append(response_str, status_line);

    // Headers
    cwist_http_header_node *curr = res->headers;
    while (curr) {
        if (curr->key->data && curr->value->data) {
            cwist_sstring_append(response_str, curr->key->data);
            cwist_sstring_append(response_str, ": ");
            cwist_sstring_append(response_str, curr->value->data);
            cwist_sstring_append(response_str, "\r\n");
        }
        curr = curr->next;
    }

    if (!headers_have_content_length(res->headers)) {
        char cl_header[64];
        snprintf(cl_header, sizeof(cl_header), "Content-Length: %zu\r\n", body_len);
        cwist_sstring_append(response_str, cl_header);
    }

    if (!headers_have_connection(res->headers)) {
        if (res->keep_alive) {
            cwist_sstring_append(response_str, "Connection: keep-alive\r\n");
        } else {
            cwist_sstring_append(response_str, "Connection: close\r\n");
        }
    }

    // End of headers
    cwist_sstring_append(response_str, "\r\n");

    // Body
    if (res->body && res->body->data) {
        cwist_sstring_append_len(response_str, res->body->data, res->body->size);
    }
    
    return response_str;
}

cwist_error_t cwist_http_send_response(int client_fd, cwist_http_response *res) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (client_fd < 0 || !res) {
        err.error.err_i16 = -1;
        return err;
    }

    cwist_sstring *response_str = cwist_http_stringify_response(res);
    if (!response_str) {
        err.error.err_i16 = -1;
        return err;
    }

    // Send
    err.error.err_i16 = 0;

    const char *p = response_str->data;
    size_t left = response_str->size;

    signal(SIGPIPE, SIG_IGN);

    while (left > 0) {
        struct pollfd pfd = { .fd = client_fd, .events = POLLOUT };
        int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
        if (ret <= 0) {
            err.error.err_i16 = -1;
            break;
        }

        ssize_t sent;
        #ifdef MSG_NOSIGNAL
        sent = send(client_fd, p, left, MSG_NOSIGNAL);
        #else
        sent = send(client_fd, p, left, 0);
        #endif

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            err.error.err_i16 = -1;
            break;
        }
        if (sent == 0) {
            err.error.err_i16 = -1;
            break;
        }

        p += (size_t)sent;
        left -= (size_t)sent;
    }

    cwist_sstring_destroy(response_str);
    return err;
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
    { ".json", "application/json" },
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

cwist_error_t cwist_http_response_send_file(cwist_http_response *res, const char *file_path, const char *content_type_hint, size_t *out_size) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!res || !file_path) {
        err.error.err_i16 = -EINVAL;
        return err;
    }

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

    if ((size_t)st.st_size > CWIST_HTTP_MAX_BODY_SIZE) {
        close(fd);
        err.error.err_i16 = -EFBIG;
        return err;
    }

    size_t file_size = (size_t)st.st_size;
    char *buffer = NULL;

    if (file_size > 0) {
        buffer = (char *)malloc(file_size);
        if (!buffer) {
            close(fd);
            err.error.err_i16 = -ENOMEM;
            return err;
        }
    }

    size_t total_read = 0;
    while (total_read < file_size) {
        ssize_t bytes = read(fd, buffer + total_read, file_size - total_read);
        if (bytes < 0) {
            if (errno == EINTR) continue;
            err.error.err_i16 = -errno;
            free(buffer);
            close(fd);
            return err;
        }
        if (bytes == 0) {
            err.error.err_i16 = -EIO;
            free(buffer);
            close(fd);
            return err;
        }
        total_read += (size_t)bytes;
    }
    close(fd);

    if (file_size > 0) {
        cwist_sstring_assign_len(res->body, buffer, file_size);
        free(buffer);
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

/* --- Socket Manipulation --- */

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
    free(cjson_error_log);
    cJSON_Delete(err_json);

    return CWIST_CREATE_SOCKET_FAILED;
  }

  if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
    cJSON *err_json = cJSON_CreateObject();
    cJSON_AddStringToObject(err_json, "err", "Failed to set up IPv4 socket options");
    char *cjson_error_log = cJSON_Print(err_json);
    perror(cjson_error_log);
    free(cjson_error_log);
    cJSON_Delete(err_json);

    return CWIST_HTTP_SETSOCKOPT_FAILED;  
  }

  sockv4->sin_family = AF_INET;
  sockv4->sin_addr.s_addr = addr;
  sockv4->sin_port = htons(port);

  if(bind(server_fd, (struct sockaddr *)sockv4, sizeof(struct sockaddr_in)) < 0) {
    cJSON *err_json = cJSON_CreateObject();
    cJSON_AddStringToObject(err_json, "err", "Failed to bind IPv4 socket");
    char *cjson_error_log = cJSON_Print(err_json);
    perror(cjson_error_log);
    free(cjson_error_log);
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
    free(cjson_error_log);
    cJSON_Delete(err_json);

    return CWIST_HTTP_LISTEN_FAILED;
  }

  return server_fd;
}

struct thread_payload {
    int client_fd;
    void (*handler_func)(int, void *);
    void *ctx;
};

static void *thread_handler(void *arg) {
    struct thread_payload *payload = (struct thread_payload *)arg;
    int client_fd = payload->client_fd;
    void (*handler_func)(int, void *) = payload->handler_func;
    void *ctx = payload->ctx;
    free(payload);
    handler_func(client_fd, ctx);
    return NULL;
}

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

cwist_error_t cwist_http_server_loop(int server_fd, cwist_server_config *config, void (*handler)(int, void *), void *ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!config || server_fd < 0 || !handler) {
        err.error.err_i16 = -1;
        return err;
    }

    if (config->use_forking) {
        while (true) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) {
                if (errno == EINTR) continue;
                err.error.err_i16 = -1;
                return err;
            }
            handle_client_forking(client_fd, handler, ctx);
        }
    }

    if (config->use_threading) {
        while (true) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) {
                if (errno == EINTR) continue;
                err.error.err_i16 = -1;
                return err;
            }
            pthread_t thread;
            struct thread_payload *payload = malloc(sizeof(*payload));
            if (!payload) {
                close(client_fd);
                continue;
            }
            payload->client_fd = client_fd;
            payload->handler_func = handler;
            payload->ctx = ctx;
            if (pthread_create(&thread, NULL, thread_handler, payload) == 0) {
                pthread_detach(thread);
            } else {
                free(payload);
                close(client_fd);
            }
        }
    }

#ifdef __linux__
    if (config->use_epoll) {
        int epoll_fd = epoll_create1(0);
        if (epoll_fd < 0) {
            err.error.err_i16 = -1;
            return err;
        }
        struct epoll_event event;
        event.events = EPOLLIN;
        event.data.fd = server_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) < 0) {
            close(epoll_fd);
            err.error.err_i16 = -1;
            return err;
        }

        while (true) {
            struct epoll_event events[16];
            int count = epoll_wait(epoll_fd, events, 16, -1);
            if (count < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < count; i++) {
                if (events[i].data.fd == server_fd) {
                    int client_fd = accept(server_fd, NULL, NULL);
                    if (client_fd >= 0) {
                        handler(client_fd, ctx);
                    }
                }
            }
        }
        close(epoll_fd);
    }
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    if (config->use_epoll) {
        int kqueue_fd = kqueue();
        if (kqueue_fd < 0) {
            err.error.err_i16 = -1;
            return err;
        }
        struct kevent change;
        EV_SET(&change, server_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
        if (kevent(kqueue_fd, &change, 1, NULL, 0, NULL) < 0) {
            close(kqueue_fd);
            err.error.err_i16 = -1;
            return err;
        }

        while (true) {
            struct kevent events[16];
            int count = kevent(kqueue_fd, NULL, 0, events, 16, NULL);
            if (count < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < count; i++) {
                if ((int)events[i].ident == server_fd) {
                    int client_fd = accept(server_fd, NULL, NULL);
                    if (client_fd >= 0) {
                        handler(client_fd, ctx);
                    }
                }
            }
        }
        close(kqueue_fd);
    }
#endif

    return cwist_accept_socket(server_fd, NULL, handler, ctx);
}
