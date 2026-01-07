#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/https.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/utils/json_builder.h> // Helper included for apps, though not strictly used here yet
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define CWIST_ROUTE_BUCKETS 127

typedef struct cwist_route_entry {
    char *path;
    bool has_params;
    cwist_http_method_t method;
    cwist_handler_func handler;
    cwist_ws_handler_func ws_handler;
    struct cwist_route_entry *next;
} cwist_route_entry;

struct cwist_route_table {
    size_t bucket_count;
    cwist_route_entry **buckets;
    cwist_route_entry *param_routes;
};

struct cwist_static_dir {
    char *url_prefix;
    char *fs_root;
    struct cwist_static_dir *next;
};

typedef struct {
    cwist_middleware_node *current_mw_node;
    cwist_handler_func final_handler;
    void *handler_data;
} mw_executor_ctx;

typedef struct {
    cwist_static_dir *mapping;
    const char *relative_ptr;
    bool use_index;
} cwist_static_request_info;

static cwist_route_table *cwist_route_table_create(void);
static void cwist_route_table_destroy(cwist_route_table *table);
static void cwist_route_table_insert(cwist_route_table *table, const char *path, cwist_http_method_t method, cwist_handler_func handler, cwist_ws_handler_func ws_handler);
static cwist_route_entry *cwist_route_table_lookup(cwist_route_table *table, cwist_http_method_t method, const char *path);
static cwist_route_entry *cwist_route_table_match_params(cwist_route_table *table, cwist_http_request *req);
static bool match_path(const char *pattern, const char *actual, cwist_query_map *params);
static void execute_chain(cwist_app *app, cwist_http_request *req, cwist_http_response *res, cwist_handler_func final_handler, void *handler_data);
static bool cwist_prepare_static(cwist_app *app, cwist_http_request *req, cwist_static_request_info *info);
static void cwist_static_handler(cwist_http_request *req, cwist_http_response *res);

static bool route_has_params(const char *path) {
    if (!path) return false;
    return strchr(path, ':') != NULL;
}

static size_t cwist_route_hash(cwist_http_method_t method, const char *path, size_t bucket_count) {
    const unsigned long long FNV_OFFSET = 1469598103934665603ULL;
    const unsigned long long FNV_PRIME = 1099511628211ULL;
    unsigned long long hash = FNV_OFFSET ^ (unsigned long long)method;
    const unsigned char *ptr = (const unsigned char *)path;
    while (ptr && *ptr) {
        hash ^= (unsigned long long)(*ptr++);
        hash *= FNV_PRIME;
    }
    return (size_t)(hash % bucket_count);
}

static cwist_route_entry *cwist_route_entry_create(const char *path, cwist_http_method_t method, cwist_handler_func handler, cwist_ws_handler_func ws_handler) {
    cwist_route_entry *entry = (cwist_route_entry *)malloc(sizeof(cwist_route_entry));
    if (!entry) return NULL;
    entry->path = strdup(path ? path : "/");
    entry->method = method;
    entry->handler = handler;
    entry->ws_handler = ws_handler;
    entry->has_params = route_has_params(entry->path);
    entry->next = NULL;
    return entry;
}

static void cwist_route_entry_free(cwist_route_entry *entry) {
    if (!entry) return;
    free(entry->path);
    free(entry);
}

static cwist_route_table *cwist_route_table_create(void) {
    cwist_route_table *table = (cwist_route_table *)malloc(sizeof(cwist_route_table));
    if (!table) return NULL;
    table->bucket_count = CWIST_ROUTE_BUCKETS;
    table->buckets = (cwist_route_entry **)calloc(table->bucket_count, sizeof(cwist_route_entry *));
    if (!table->buckets) {
        free(table);
        return NULL;
    }
    table->param_routes = NULL;
    return table;
}

static void cwist_route_table_destroy(cwist_route_table *table) {
    if (!table) return;
    for (size_t i = 0; i < table->bucket_count; i++) {
        cwist_route_entry *curr = table->buckets[i];
        while (curr) {
            cwist_route_entry *next = curr->next;
            cwist_route_entry_free(curr);
            curr = next;
        }
    }
    free(table->buckets);

    cwist_route_entry *param = table->param_routes;
    while (param) {
        cwist_route_entry *next = param->next;
        cwist_route_entry_free(param);
        param = next;
    }
    free(table);
}

static void cwist_route_table_insert(cwist_route_table *table, const char *path, cwist_http_method_t method, cwist_handler_func handler, cwist_ws_handler_func ws_handler) {
    if (!table || !path) return;
    cwist_route_entry *entry = cwist_route_entry_create(path, method, handler, ws_handler);
    if (!entry) return;

    if (entry->has_params) {
        entry->next = table->param_routes;
        table->param_routes = entry;
        return;
    }

    size_t idx = cwist_route_hash(method, entry->path, table->bucket_count);
    cwist_route_entry **bucket = &table->buckets[idx];
    cwist_route_entry *curr = *bucket;
    while (curr) {
        if (!curr->has_params && curr->method == method && strcmp(curr->path, entry->path) == 0) {
            curr->handler = handler;
            curr->ws_handler = ws_handler;
            cwist_route_entry_free(entry);
            return;
        }
        curr = curr->next;
    }

    entry->next = *bucket;
    *bucket = entry;
}

static cwist_route_entry *cwist_route_table_lookup(cwist_route_table *table, cwist_http_method_t method, const char *path) {
    if (!table || !path) return NULL;
    size_t idx = cwist_route_hash(method, path, table->bucket_count);
    cwist_route_entry *curr = table->buckets[idx];
    while (curr) {
        if (curr->method == method && strcmp(curr->path, path) == 0) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

static cwist_route_entry *cwist_route_table_match_params(cwist_route_table *table, cwist_http_request *req) {
    if (!table || !req || !req->path || !req->path->data) return NULL;
    cwist_route_entry *curr = table->param_routes;
    while (curr) {
        if (curr->method == req->method) {
            if (match_path(curr->path, req->path->data, req->path_params)) {
                return curr;
            }
        }
        curr = curr->next;
    }
    return NULL;
}

static bool cwist_path_has_parent_ref(const char *path) {
    if (!path) return false;
    const char *cursor = path;
    while (*cursor) {
        if (*cursor == '.') {
            char prev = (cursor == path) ? '/' : *(cursor - 1);
            char next = *(cursor + 1);
            char next_next = *(cursor + 2);
            if (prev == '/' && next == '.' && (next_next == '/' || next_next == '\0')) {
                return true;
            }
        }
        cursor++;
    }
    return false;
}

static bool cwist_static_match_entry(const cwist_static_dir *entry, const char *req_path, const char **relative_ptr, bool *use_index) {
    if (!entry || !req_path || req_path[0] == '\0') return false;
    size_t prefix_len = strlen(entry->url_prefix);
    if (prefix_len == 0) return false;
    if (prefix_len == 1 && entry->url_prefix[0] == '/') {
        if (req_path[0] != '/') return false;
        if (req_path[1] == '\0') {
            if (use_index) *use_index = true;
            if (relative_ptr) *relative_ptr = NULL;
        } else {
            if (use_index) *use_index = false;
            if (relative_ptr) *relative_ptr = req_path + 1;
        }
        return true;
    }

    if (strncmp(req_path, entry->url_prefix, prefix_len) != 0) {
        return false;
    }

    char separator = req_path[prefix_len];
    if (separator == '\0') {
        if (use_index) *use_index = true;
        if (relative_ptr) *relative_ptr = NULL;
        return true;
    }
    if (separator != '/') {
        return false;
    }
    if (use_index) *use_index = false;
    if (relative_ptr) *relative_ptr = req_path + prefix_len + 1;
    return true;
}

static bool cwist_prepare_static(cwist_app *app, cwist_http_request *req, cwist_static_request_info *info) {
    if (!app || !req || !req->path || !req->path->data) return false;
    if (!app->static_dirs) return false;
    if (req->method != CWIST_HTTP_GET && req->method != CWIST_HTTP_HEAD) return false;

    cwist_static_dir *entry = app->static_dirs;
    const char *path = req->path->data;
    while (entry) {
        bool use_index = false;
        const char *relative = NULL;
        if (cwist_static_match_entry(entry, path, &relative, &use_index)) {
            if (info) {
                info->mapping = entry;
                info->relative_ptr = relative;
                info->use_index = use_index;
            }
            return true;
        }
        entry = entry->next;
    }
    return false;
}

static char *cwist_normalize_prefix(const char *prefix) {
    if (!prefix || prefix[0] == '\0') {
        return strdup("/");
    }

    size_t len = strlen(prefix);
    bool needs_leading_slash = prefix[0] != '/';
    char *buffer = (char *)malloc(len + needs_leading_slash + 1);
    if (!buffer) {
        return NULL;
    }

    if (needs_leading_slash) {
        buffer[0] = '/';
        memcpy(buffer + 1, prefix, len + 1);
        len += 1;
    } else {
        memcpy(buffer, prefix, len + 1);
    }

    while (len > 1 && buffer[len - 1] == '/') {
        buffer[len - 1] = '\0';
        len--;
    }

    return buffer;
}

static char *cwist_normalize_directory(const char *directory) {
    if (!directory || directory[0] == '\0') {
        return strdup(".");
    }
    size_t len = strlen(directory);
    while (len > 1 && directory[len - 1] == '/') {
        len--;
    }
    char *copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, directory, len);
    copy[len] = '\0';
    return copy;
}

static void cwist_static_handler(cwist_http_request *req, cwist_http_response *res) {
    mw_executor_ctx *ctx = (mw_executor_ctx *)req->private_data;
    cwist_static_request_info *info = ctx ? (cwist_static_request_info *)ctx->handler_data : NULL;
    if (!info || !info->mapping) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "Static handler misconfigured");
        return;
    }

    char relative_buf[PATH_MAX];
    if (info->use_index || !info->relative_ptr || info->relative_ptr[0] == '\0') {
        snprintf(relative_buf, sizeof(relative_buf), "index.html");
    } else {
        snprintf(relative_buf, sizeof(relative_buf), "%s", info->relative_ptr);
    }
    relative_buf[PATH_MAX - 1] = '\0';

    if (cwist_path_has_parent_ref(relative_buf)) {
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Directory traversal blocked");
        return;
    }

    char fs_path[PATH_MAX];
    int written = snprintf(fs_path, sizeof(fs_path), "%s/%s", info->mapping->fs_root, relative_buf);
    if (written < 0 || written >= (int)sizeof(fs_path)) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "Static path too long");
        return;
    }

    size_t file_size = 0;
    cwist_error_t ferr = cwist_http_response_send_file(res, fs_path, NULL, &file_size);
    if (ferr.error.err_i16 == 0) {
        if (req->method == CWIST_HTTP_HEAD) {
            char len_buf[32];
            snprintf(len_buf, sizeof(len_buf), "%zu", file_size);
            if (!cwist_http_header_get(res->headers, "Content-Length")) {
                cwist_http_header_add(&res->headers, "Content-Length", len_buf);
            }
            cwist_sstring_assign(res->body, "");
        }
        return;
    }

    if (ferr.error.err_i16 == -ENOENT) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Not Found");
    } else if (ferr.error.err_i16 == -EISDIR) {
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Directory listing not allowed");
    } else if (ferr.error.err_i16 == -EFBIG) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "Static file too large");
    } else {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "Failed to read static file");
    }
}
#include <limits.h>
#include <errno.h>

cwist_app *cwist_app_create(void) {
    cwist_app *app = (cwist_app *)malloc(sizeof(cwist_app));
    if (!app) return NULL;
    
    app->port = 8080;
    app->use_ssl = false;
    app->cert_path = NULL;
    app->key_path = NULL;
    app->router = cwist_route_table_create();
    if (!app->router) {
        free(app);
        return NULL;
    }
    app->middlewares = NULL;
    app->ssl_ctx = NULL;
    app->error_handler = NULL;
    app->static_dirs = NULL;
    app->db = NULL;
    app->db_path = NULL;
    
    return app;
}

void cwist_app_use(cwist_app *app, cwist_middleware_func mw) {
    if (!app || !mw) return;
    cwist_middleware_node *node = malloc(sizeof(cwist_middleware_node));
    node->func = mw;
    node->next = NULL;

    if (!app->middlewares) {
        app->middlewares = node;
    } else {
        cwist_middleware_node *curr = app->middlewares;
        while (curr->next) curr = curr->next;
        curr->next = node;
    }
}

void cwist_app_set_error_handler(cwist_app *app, cwist_error_handler_func handler) {
    if (app) app->error_handler = handler;
}

void cwist_app_destroy(cwist_app *app) {
    if (!app) return;
    if (app->cert_path) free(app->cert_path);
    if (app->key_path) free(app->key_path);
    if (app->ssl_ctx) cwist_https_destroy_context(app->ssl_ctx);

    cwist_route_table_destroy(app->router);

    cwist_middleware_node *curr_m = app->middlewares;
    while (curr_m) {
        cwist_middleware_node *next = curr_m->next;
        free(curr_m);
        curr_m = next;
    }

    cwist_static_dir *curr_s = app->static_dirs;
    while (curr_s) {
        cwist_static_dir *next = curr_s->next;
        free(curr_s->url_prefix);
        free(curr_s->fs_root);
        free(curr_s);
        curr_s = next;
    }

    if (app->db) {
        cwist_db_close(app->db);
    }
    if (app->db_path) {
        free(app->db_path);
    }
    
    free(app);
}

static void mw_next_wrapper(cwist_http_request *req, cwist_http_response *res) {
    mw_executor_ctx *ctx = (mw_executor_ctx *)req->private_data;
    if (!ctx) return;

    if (ctx->current_mw_node) {
        cwist_middleware_node *node = ctx->current_mw_node;
        // Advance the chain for the next "next" call
        ctx->current_mw_node = node->next;
        node->func(req, res, mw_next_wrapper);
    } else if (ctx->final_handler) {
        ctx->final_handler(req, res);
    }
}

static void execute_chain(cwist_app *app, cwist_http_request *req, cwist_http_response *res, cwist_handler_func final_handler, void *handler_data) {
    mw_executor_ctx ctx = { app->middlewares, final_handler, handler_data };
    req->private_data = &ctx;
    mw_next_wrapper(req, res);
    req->private_data = NULL;
}

cwist_error_t cwist_app_use_https(cwist_app *app, const char *cert_path, const char *key_path) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!app || !cert_path || !key_path) {
        err.error.err_i16 = -1;
        return err;
    }

    app->use_ssl = true;
    app->cert_path = strdup(cert_path);
    app->key_path = strdup(key_path);

    return cwist_https_init_context(&app->ssl_ctx, cert_path, key_path);
}

cwist_error_t cwist_app_use_db(cwist_app *app, const char *db_path) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!app || !db_path) {
        err.error.err_i16 = -1;
        return err;
    }

    cwist_db *db = NULL;
    err = cwist_db_open(&db, db_path);
    if (err.error.err_i16 < 0) {
        return err;
    }

    if (app->db) {
        cwist_db_close(app->db);
    }
    if (app->db_path) {
        free(app->db_path);
    }

    app->db = db;
    app->db_path = strdup(db_path);
    return err;
}

cwist_db *cwist_app_get_db(cwist_app *app) {
    if (!app) return NULL;
    return app->db;
}

cwist_error_t cwist_app_static(cwist_app *app, const char *url_prefix, const char *directory) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!app || !url_prefix || !directory) {
        err.error.err_i16 = -1;
        return err;
    }

    char *normalized = cwist_normalize_prefix(url_prefix);
    if (!normalized) {
        err.error.err_i16 = -1;
        return err;
    }

    char *resolved = cwist_normalize_directory(directory);
    if (!resolved) {
        free(normalized);
        err.error.err_i16 = -1;
        return err;
    }

    cwist_static_dir *entry = (cwist_static_dir *)malloc(sizeof(cwist_static_dir));
    if (!entry) {
        free(normalized);
        free(resolved);
        err.error.err_i16 = -1;
        return err;
    }

    entry->url_prefix = normalized;
    entry->fs_root = resolved;
    entry->next = app->static_dirs;
    app->static_dirs = entry;

    err.error.err_i16 = 0;
    return err;
}

static void add_route(cwist_app *app, const char *path, cwist_http_method_t method, cwist_handler_func handler) {
    if (!app || !app->router || !path) return;
    cwist_route_table_insert(app->router, path, method, handler, NULL);
}

void cwist_app_get(cwist_app *app, const char *path, cwist_handler_func handler) {
    add_route(app, path, CWIST_HTTP_GET, handler);
}

void cwist_app_post(cwist_app *app, const char *path, cwist_handler_func handler) {
    add_route(app, path, CWIST_HTTP_POST, handler);
}

void cwist_app_ws(cwist_app *app, const char *path, cwist_ws_handler_func handler) {
    if (!app || !app->router || !path) return;
    cwist_route_table_insert(app->router, path, CWIST_HTTP_GET, NULL, handler);
}

static bool match_path(const char *pattern, const char *actual, cwist_query_map *params) {
    char p[256], a[256];
    strncpy(p, pattern, 255);
    strncpy(a, actual, 255);
    p[255] = a[255] = '\0';

    if (params) {
        cwist_query_map_clear(params);
    }

    char *saveptr_p, *saveptr_a;
    char *tok_p = strtok_r(p, "/", &saveptr_p);
    char *tok_a = strtok_r(a, "/", &saveptr_a);

    while (tok_p && tok_a) {
        if (tok_p[0] == ':') {
            // Path Parameter
            cwist_query_map_set(params, tok_p + 1, tok_a);
        } else if (strcmp(tok_p, tok_a) != 0) {
            return false;
        }
        tok_p = strtok_r(NULL, "/", &saveptr_p);
        tok_a = strtok_r(NULL, "/", &saveptr_a);
    }

    return tok_p == NULL && tok_a == NULL;
}

// Internal Router Logic
static void internal_route_handler(cwist_app *app, cwist_http_request *req, cwist_http_response *res) {
    if (!req || !app || !app->router) return;

    cwist_static_request_info static_info = {0};
    if (cwist_prepare_static(app, req, &static_info)) {
        execute_chain(app, req, res, cwist_static_handler, &static_info);
        return;
    }

    const char *path = (req->path && req->path->data) ? req->path->data : "/";
    cwist_route_entry *found_route = cwist_route_table_lookup(app->router, req->method, path);

    if (found_route && req->path_params) {
        cwist_query_map_clear(req->path_params);
    }

    if (!found_route) {
        found_route = cwist_route_table_match_params(app->router, req);
    }

    if (found_route) {
        if (found_route->ws_handler) {
            if (req->client_fd >= 0) {
                cwist_websocket *ws = cwist_websocket_upgrade(req, req->client_fd);
                if (ws) {
                    found_route->ws_handler(ws);
                    cwist_websocket_destroy(ws);
                } else {
                    res->status_code = CWIST_HTTP_BAD_REQUEST;
                    cwist_sstring_assign(res->body, "WebSocket Upgrade Failed");
                }
            }
        } else {
            execute_chain(app, req, res, found_route->handler, NULL);
        }
    } else {
        if (app->error_handler) {
            app->error_handler(req, res, CWIST_HTTP_NOT_FOUND);
        } else {
            res->status_code = CWIST_HTTP_NOT_FOUND;
            cwist_sstring_assign(res->body, "404 Not Found");
        }
    }
}

static void static_ssl_handler(cwist_https_connection *conn, void *ctx) {
    cwist_app *app = (cwist_app *)ctx;
    cwist_http_request *req = cwist_https_receive_request(conn);
    if (!req) return;
    req->app = app;
    req->db = app->db;
    
    cwist_http_response *res = cwist_http_response_create();
    internal_route_handler(app, req, res);
    
    cwist_https_send_response(conn, res);
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
}

static void static_http_handler(int client_fd, void *ctx) {
    cwist_app *app = (cwist_app *)ctx;
    char *read_buf = malloc(CWIST_HTTP_READ_BUFFER_SIZE);
    if (!read_buf) {
        close(client_fd);
        return;
    }
    size_t buf_len = 0;
    read_buf[0] = '\0';

    while (true) {
        cwist_http_request *req = cwist_http_receive_request(client_fd, read_buf, CWIST_HTTP_READ_BUFFER_SIZE, &buf_len);
        if (!req) {
            break;
        }
        req->client_fd = client_fd;
        req->app = app;
        req->db = app->db;

        cwist_http_response *res = cwist_http_response_create();
        if (!res) {
            cwist_http_request_destroy(req);
            break;
        }
        
        internal_route_handler(app, req, res);
        
        bool keep_alive = req->keep_alive && res->keep_alive;
        
        if (!req->upgraded) {
            if (cwist_http_send_response(client_fd, res).error.err_i16 < 0) {
                cwist_http_response_destroy(res);
                cwist_http_request_destroy(req);
                break;
            }
        }
        
        cwist_http_response_destroy(res);
        cwist_http_request_destroy(req);
        
        if (!keep_alive || req->upgraded) {
            break;
        }
    }
    
    free(read_buf);
    close(client_fd);
}

int cwist_app_listen(cwist_app *app, int port) {
    if (!app) return -1;
    app->port = port;
    
    struct sockaddr_in addr;
    int server_fd = cwist_make_socket_ipv4(&addr, "0.0.0.0", port, 128);
    if (server_fd < 0) {
        perror("Failed to bind port");
        return -1;
    }
    
    printf("CWIST App running on port %d (SSL: %s)\n", port, app->use_ssl ? "On" : "Off");
    
    if (app->use_ssl) {
        if (!app->ssl_ctx) {
            fprintf(stderr, "SSL enabled but context not initialized.\n");
            return -1;
        }
        cwist_https_server_loop(server_fd, app->ssl_ctx, static_ssl_handler, app);
    } else {
        cwist_server_config config = { .use_forking = false, .use_threading = true, .use_epoll = false };
        cwist_http_server_loop(server_fd, &config, static_http_handler, app);
    }
    
    return 0;
}
