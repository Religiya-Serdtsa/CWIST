#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <cwist/sys/app/app.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/https.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/db/nuke_db.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/utils/json_builder.h> // Helper included for apps, though not strictly used here yet
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include <ttak/mem/mem.h>
#include <ttak/timing/timing.h>

#define CWIST_ROUTE_BUCKETS 127
#define CWIST_STATIC_RETIRE_NS TT_SECOND(5)

/**
 * @brief Read the current libttak tick count used for static-file retirement deadlines.
 * @return Monotonic tick value compatible with libttak memory APIs.
 */
static inline uint64_t cwist_mem_now(void) {
    return ttak_get_tick_count();
}

/**
 * @brief Check whether the static-file memory cache can admit a payload after reclamation.
 * @param mem Static-file memory manager.
 * @param incoming Size of the candidate payload.
 * @param reclaimable Bytes that could be reclaimed from an existing entry.
 * @return true when the projected usage fits inside the configured capacity.
 */
static bool cwist_mem_has_capacity(cwist_fix_server_mem *mem, size_t incoming, size_t reclaimable) {
    if (!mem || mem->total_capacity == 0) {
        return true;
    }
    if (incoming > mem->total_capacity) {
        return false;
    }
    size_t used = mem->current_used;
    if (reclaimable > used) {
        reclaimable = used;
    }
    size_t projected = used - reclaimable + incoming;
    return projected <= mem->total_capacity;
}

/**
 * @brief Reserve one metadata slot in the static-file registry, growing the array when needed.
 * @param mem Static-file memory manager.
 * @return Pointer to the claimed entry slot, or NULL on allocation failure.
 */
static cwist_file_t *cwist_mem_claim_entry(cwist_fix_server_mem *mem) {
    if (!mem) return NULL;
    if (mem->file_count >= mem->files_capacity) {
        size_t new_cap = mem->files_capacity == 0 ? 16 : mem->files_capacity * 2;
        cwist_file_t *new_files = cwist_realloc(mem->files, new_cap * sizeof(cwist_file_t));
        if (!new_files) {
            return NULL;
        }
        mem->files = new_files;
        mem->files_capacity = new_cap;
    }
    cwist_file_t *entry = &mem->files[mem->file_count];
    memset(entry, 0, sizeof(*entry));
    mem->file_count++;
    return entry;
}

/**
 * @brief Load a filesystem object into libttak-managed memory and track its tree node.
 * @param mem Static-file memory manager.
 * @param fs_path Filesystem path to read.
 * @param size Number of bytes to load.
 * @param data_out Output pointer receiving the allocated payload.
 * @param node_out Output pointer receiving the libttak tree node.
 * @return true when the payload was loaded and registered successfully.
 */
static bool cwist_mem_create_payload(cwist_fix_server_mem *mem, const char *fs_path, size_t size, void **data_out, ttak_mem_node_t **node_out) {
    if (!mem || !fs_path || !data_out || !node_out) return false;

    void *buffer = ttak_mem_alloc_safe(size ? size : 1, __TTAK_UNSAFE_MEM_FOREVER__, cwist_mem_now(), true, false, true, true, TTAK_MEM_DEFAULT);
    if (!buffer) {
        fprintf(stderr, "[StaticMem] Failed to allocate %zu bytes via libttak for %s\n", size, fs_path);
        return false;
    }

    FILE *f = fopen(fs_path, "rb");
    if (!f) {
        fprintf(stderr, "[StaticMem] Failed to open %s\n", fs_path);
        ttak_mem_free(buffer);
        return false;
    }

    size_t to_read = size;
    if (to_read > 0) {
        size_t read = fread(buffer, 1, to_read, f);
        if (read != to_read) {
            fprintf(stderr, "[StaticMem] Short read for %s (expected %zu, got %zu)\n", fs_path, to_read, read);
            fclose(f);
            ttak_mem_free(buffer);
            return false;
        }
    }
    fclose(f);

    ttak_mem_node_t *node = ttak_mem_tree_add(&mem->file_tree, buffer, size ? size : 1, __TTAK_UNSAFE_MEM_FOREVER__, true);
    if (!node) {
        ttak_mem_free(buffer);
        return false;
    }

    *data_out = buffer;
    *node_out = node;
    return true;
}

/**
 * @brief Retire an old static-file node after a grace period so in-flight reads can finish.
 * @param mem Static-file memory manager.
 * @param node Previous libttak node to release.
 */
static void cwist_mem_release_node_delayed(cwist_fix_server_mem *mem, ttak_mem_node_t *node) {
    if (!mem || !node) return;
    uint64_t now = cwist_mem_now();
    pthread_mutex_lock(&node->lock);
    node->expires_tick = now + mem->retire_grace_ns;
    pthread_mutex_unlock(&node->lock);
    ttak_mem_node_release(node);
}

/**
 * @brief Populate a registry entry with a freshly loaded static-file payload.
 * @param mem Static-file memory manager.
 * @param entry Registry entry to fill.
 * @param fs_path Filesystem path associated with the payload.
 * @param st Stat information for the file.
 * @param data Loaded file bytes.
 * @param node Libttak node tracking the payload.
 * @return true when the entry was attached successfully.
 */
static bool cwist_mem_attach_entry(cwist_fix_server_mem *mem, cwist_file_t *entry, const char *fs_path, const struct stat *st, void *data, ttak_mem_node_t *node) {
    if (!mem || !entry || !fs_path || !st) return false;
    char *path_copy = cwist_strdup(fs_path);
    if (!path_copy) {
        ttak_mem_tree_remove(&mem->file_tree, node);
        return false;
    }

    entry->path = NULL;
    entry->fs_path = path_copy;
    entry->data = data;
    entry->size = st->st_size;
    entry->last_mod = st->st_mtime;
    entry->node = node;

    mem->current_used += st->st_size;
    return true;
}

/**
 * @brief Register a new static file in the fixed-memory cache.
 * @param mem Static-file memory manager.
 * @param fs_path Filesystem path to cache.
 * @param st Stat information describing the file.
 * @return true when the file was admitted to the cache.
 */
static bool cwist_mem_register_file(cwist_fix_server_mem *mem, const char *fs_path, const struct stat *st) {
    if (!mem || !fs_path || !st) return false;
    if (!cwist_mem_has_capacity(mem, st->st_size, 0)) {
        fprintf(stderr, "[StaticMem] Skipping %s (size %zu exceeds capacity)\n", fs_path, st->st_size);
        return false;
    }
    cwist_file_t *entry = cwist_mem_claim_entry(mem);
    if (!entry) {
        fprintf(stderr, "[StaticMem] Failed to allocate metadata entry for %s\n", fs_path);
        return false;
    }
    void *data = NULL;
    ttak_mem_node_t *node = NULL;
    if (!cwist_mem_create_payload(mem, fs_path, st->st_size, &data, &node)) {
        mem->file_count--;
        return false;
    }
    if (!cwist_mem_attach_entry(mem, entry, fs_path, st, data, node)) {
        mem->file_count--;
        return false;
    }
    return true;
}

/**
 * @brief Reload a cached static file after detecting a modification on disk.
 * @param mem Static-file memory manager.
 * @param entry Existing cache entry to refresh.
 * @param st Updated stat information for the file.
 * @return true when the file was refreshed successfully.
 */
static bool cwist_mem_refresh_file(cwist_fix_server_mem *mem, cwist_file_t *entry, const struct stat *st) {
    if (!mem || !entry || !st) return false;
    size_t reclaimable = entry->size;
    if (!cwist_mem_has_capacity(mem, st->st_size, reclaimable)) {
        fprintf(stderr, "[StaticMem] OOM reloading %s (%zu bytes)\n", entry->fs_path, (size_t)st->st_size);
        return false;
    }

    void *data = NULL;
    ttak_mem_node_t *node = NULL;
    if (!cwist_mem_create_payload(mem, entry->fs_path, st->st_size, &data, &node)) {
        return false;
    }

    ttak_mem_node_t *old_node = entry->node;
    size_t old_size = entry->size;

    entry->data = data;
    entry->node = node;
    entry->size = st->st_size;
    entry->last_mod = st->st_mtime;

    if (mem->current_used >= old_size) {
        mem->current_used -= old_size;
    } else {
        mem->current_used = 0;
    }
    mem->current_used += st->st_size;

    if (old_node) {
        cwist_mem_release_node_delayed(mem, old_node);
    }
    return true;
}


typedef struct cwist_route_entry {
    char *path;
    bool has_params;
    cwist_http_method_t method;
    cwist_handler_func handler;
    cwist_ws_handler_func ws_handler;
    cwist_endpoint_opt_t opts;
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
static void cwist_route_table_insert(cwist_route_table *table,
                                     const char *path,
                                     cwist_http_method_t method,
                                     cwist_handler_func handler,
                                     cwist_ws_handler_func ws_handler,
                                     cwist_endpoint_opt_t opts);
static cwist_route_entry *cwist_route_table_lookup(cwist_route_table *table, cwist_http_method_t method, const char *path);
static cwist_route_entry *cwist_route_table_match_params(cwist_route_table *table, cwist_http_request *req);
static bool match_path(const char *pattern, const char *actual, cwist_query_map *params);
static void execute_chain(cwist_app *app, cwist_http_request *req, cwist_http_response *res, cwist_handler_func final_handler, void *handler_data);
static bool cwist_prepare_static(cwist_app *app, cwist_http_request *req, cwist_static_request_info *info);
static void cwist_static_handler(cwist_http_request *req, cwist_http_response *res);

/**
 * @brief Detect whether a route pattern contains colon-prefixed path parameters.
 * @param path Route pattern to inspect.
 * @return true when the path contains parameter segments.
 */
static bool route_has_params(const char *path) {
    if (!path) return false;
    return strchr(path, ':') != NULL;
}

/**
 * @brief Hash a method/path pair into the fixed route-table bucket space.
 * @param method HTTP method associated with the route.
 * @param path Route path string.
 * @param bucket_count Number of buckets in the route table.
 * @return Bucket index for the route.
 */
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

static cwist_route_entry *cwist_route_entry_create(const char *path,
                                                   cwist_http_method_t method,
                                                   cwist_handler_func handler,
                                                   cwist_ws_handler_func ws_handler,
                                                   cwist_endpoint_opt_t opts) {
    cwist_route_entry *entry = (cwist_route_entry *)cwist_alloc(sizeof(cwist_route_entry));
    if (!entry) return NULL;
    entry->path = cwist_strdup(path ? path : "/");
    entry->method = method;
    entry->handler = handler;
    entry->ws_handler = ws_handler;
    entry->opts = opts;
    entry->has_params = route_has_params(entry->path);
    entry->next = NULL;
    return entry;
}

/**
 * @brief Destroy one route entry and its owned path string.
 * @param entry Route entry to release.
 */
static void cwist_route_entry_free(cwist_route_entry *entry) {
    if (!entry) return;
    cwist_free(entry->path);
    cwist_free(entry);
}

static cwist_route_table *cwist_route_table_create(void) {
    cwist_route_table *table = (cwist_route_table *)cwist_alloc(sizeof(cwist_route_table));
    if (!table) return NULL;
    table->bucket_count = CWIST_ROUTE_BUCKETS;
    table->buckets = (cwist_route_entry **)cwist_alloc_array(table->bucket_count, sizeof(cwist_route_entry *));
    if (!table->buckets) {
        cwist_free(table);
        return NULL;
    }
    table->param_routes = NULL;
    return table;
}

/**
 * @brief Destroy the route table, including static and parameterized route chains.
 * @param table Route table to release.
 */
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
    cwist_free(table->buckets);

    cwist_route_entry *param = table->param_routes;
    while (param) {
        cwist_route_entry *next = param->next;
        cwist_route_entry_free(param);
        param = next;
    }
    cwist_free(table);
}

static void cwist_route_table_insert(cwist_route_table *table,
                                     const char *path,
                                     cwist_http_method_t method,
                                     cwist_handler_func handler,
                                     cwist_ws_handler_func ws_handler,
                                     cwist_endpoint_opt_t opts) {
    if (!table || !path) return;
    cwist_route_entry *entry = cwist_route_entry_create(path, method, handler, ws_handler, opts);
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
            curr->opts = opts;
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
    
    // Tiny string optimization: if length <= 8, cast to uint64 and compare in one shot
    // Note: We need to handle potential access beyond string end safely.
    // However, simplest heuristic is checking length first.
    // Actually, we can just check length.
    
    size_t path_len = strlen(path);
    uint64_t path_u64 = 0;
    bool use_fast_path = (path_len <= 8);
    if (use_fast_path) {
        memcpy(&path_u64, path, path_len); // Safe copy
    }

    while (curr) {
        if (curr->method == method) {
            if (use_fast_path) {
                 // Fast path check
                 size_t curr_len = strlen(curr->path);
                 if (curr_len == path_len) {
                     uint64_t curr_u64 = 0;
                     memcpy(&curr_u64, curr->path, curr_len);
                     if (path_u64 == curr_u64) return curr;
                 }
            } else {
                if (strcmp(curr->path, path) == 0) {
                    return curr;
                }
            }
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

/**
 * @brief Reject static-file paths that attempt parent-directory traversal.
 * @param path Relative path component derived from the request.
 * @return true when the path contains `..` traversal segments.
 */
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

/**
 * @brief Match a request path against one configured static-directory mapping.
 * @param entry Static-directory mapping candidate.
 * @param req_path Request path to inspect.
 * @param relative_ptr Output pointer receiving the unmatched suffix inside the mapping.
 * @param use_index Output flag indicating whether an index file should be served.
 * @return true when the request is covered by the mapping.
 */
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

/**
 * @brief Resolve a request path to one configured static directory before routing.
 * @param app Application that owns the static mappings.
 * @param req Incoming request to inspect.
 * @param info Output structure receiving the resolved mapping details.
 * @return true when the request should be served by the static-file handler.
 */
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

/**
 * @brief Recursively scan a static root directory to size or populate the fixed-memory cache.
 * @param fs_root Filesystem directory to scan.
 * @param total_size Running byte total accumulated during the scan.
 * @param mem Static-file memory manager to populate when not in dry-run mode.
 * @param dry_run When true, only compute the required capacity.
 */
static void cwist_scan_recursive(const char *fs_root, size_t *total_size, cwist_fix_server_mem *mem, bool dry_run) {
    DIR *d = opendir(fs_root);
    if (!d) return;

    struct dirent *dir;
    char full_path[PATH_MAX];
    struct stat st;

    while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;

        snprintf(full_path, sizeof(full_path), "%s/%s", fs_root, dir->d_name);
        if (stat(full_path, &st) == -1) continue;

        if (S_ISDIR(st.st_mode)) {
            cwist_scan_recursive(full_path, total_size, mem, dry_run);
        } else if (S_ISREG(st.st_mode)) {
            if (dry_run) {
                if (total_size) *total_size += st.st_size;
            } else if (mem) {
                if (!cwist_mem_register_file(mem, full_path, &st)) {
                    fprintf(stderr, "[StaticMem] Failed to load %s\n", full_path);
                }
            }
        }
    }
    closedir(d);
}

/**
 * @brief Initialize the static-file fixed-memory cache based on configured directories.
 * @param app Application whose static mappings should be scanned and cached.
 */
static void cwist_mem_init(cwist_app *app) {
    if (!app || !app->static_dirs) return;
    
    app->mem_manager = cwist_alloc(sizeof(cwist_fix_server_mem));
    app->mem_manager->check_interval_ms = 2000; 
    pthread_mutex_init(&app->mem_manager->lock, NULL);
    app->mem_manager->retire_grace_ns = CWIST_STATIC_RETIRE_NS;
    ttak_mem_tree_init(&app->mem_manager->file_tree);

    size_t total_size = 0;
    cwist_static_dir *curr = app->static_dirs;
    while (curr) {
        cwist_scan_recursive(curr->fs_root, &total_size, NULL, true);
        curr = curr->next;
    }

    if (app->max_mem_space > 0) {
        app->mem_manager->total_capacity = app->max_mem_space;
    } else {
        if (total_size == 0) total_size = CWIST_MIB(1); 
        app->mem_manager->total_capacity = total_size * 2;
    }

    app->mem_manager->current_used = 0;
    
    // Load files
    curr = app->static_dirs;
    while (curr) {
        cwist_scan_recursive(curr->fs_root, NULL, app->mem_manager, false);
        curr = curr->next;
    }
    
    printf("Server Memory Initialized: %zu used / %zu total bytes (%zu files)\n", 
           app->mem_manager->current_used, app->mem_manager->total_capacity, app->mem_manager->file_count);
}

static void *cwist_mem_watcher(void *arg) {
    cwist_app *app = (cwist_app *)arg;
    cwist_fix_server_mem *mem = app->mem_manager;
    
    while (mem->watcher_running) {
        usleep(mem->check_interval_ms * 1000);
        
        pthread_mutex_lock(&mem->lock);
        for (size_t i = 0; i < mem->file_count; i++) {
            cwist_file_t *f = &mem->files[i];
            struct stat st;
            if (stat(f->fs_path, &st) == 0) {
                if (st.st_mtime > f->last_mod) {
                    if (cwist_mem_refresh_file(mem, f, &st)) {
                        printf("[Hot Reload] Updated: %s\n", f->fs_path);
                    }
                }
            }
        }
        pthread_mutex_unlock(&mem->lock);
    }
    return NULL;
}

static cwist_file_t *cwist_mem_get_file(cwist_fix_server_mem *mem, const char *fs_path) {
    if (!mem || !fs_path) return NULL;
    for (size_t i = 0; i < mem->file_count; i++) {
        if (strcmp(mem->files[i].fs_path, fs_path) == 0) {
            return &mem->files[i];
        }
    }
    return NULL;
}

static char *cwist_normalize_prefix(const char *prefix) {
    if (!prefix || prefix[0] == '\0') {
        return cwist_strdup("/");
    }

    size_t len = strlen(prefix);
    bool needs_leading_slash = prefix[0] != '/';
    char *buffer = (char *)cwist_alloc(len + needs_leading_slash + 1);
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
        return cwist_strdup(".");
    }
    size_t len = strlen(directory);
    while (len > 1 && directory[len - 1] == '/') {
        len--;
    }
    char *copy = (char *)cwist_alloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, directory, len);
    copy[len] = '\0';
    return copy;
}

/**
 * @brief Cleanup hook used when a response borrows a static-file cache payload.
 * @param ptr Borrowed body pointer.
 * @param len Borrowed body length.
 * @param ctx Cache entry that owns the libttak node.
 */
static void cwist_static_release_body(const void *ptr, size_t len, void *ctx) {
    (void)ptr;
    (void)len;
    ttak_mem_node_t *node = (ttak_mem_node_t *)ctx;
    if (node) {
        ttak_mem_node_release(node);
    }
}

/**
 * @brief Serve a static file response from the fixed-memory cache or disk fallback.
 * @param req Incoming request targeting a static mapping.
 * @param res Response object to populate.
 */
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

    cwist_app *app = req->app;
    if (!app || !app->mem_manager) {
         res->status_code = CWIST_HTTP_INTERNAL_ERROR;
         cwist_sstring_assign(res->body, "Server memory not initialized");
         return;
    }

    cwist_fix_server_mem *mem = app->mem_manager;
    pthread_mutex_lock(&mem->lock);
    cwist_file_t *file = cwist_mem_get_file(mem, fs_path);
    
    if (file) {
        // Simple MIME guess
        const char *dot = strrchr(fs_path, '.');
        const char *mime = "application/octet-stream";
        if (dot) {
            if (strcasecmp(dot, ".html") == 0) mime = "text/html; charset=utf-8";
            else if (strcasecmp(dot, ".css") == 0) mime = "text/css; charset=utf-8";
            else if (strcasecmp(dot, ".js") == 0) mime = "application/javascript";
            else if (strcasecmp(dot, ".json") == 0) mime = "application/json";
            else if (strcasecmp(dot, ".png") == 0) mime = "image/png";
            else if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) mime = "image/jpeg";
            else if (strcasecmp(dot, ".gif") == 0) mime = "image/gif";
            else if (strcasecmp(dot, ".svg") == 0) mime = "image/svg+xml";
            else if (strcasecmp(dot, ".txt") == 0) mime = "text/plain; charset=utf-8";
        }

        if (req->method == CWIST_HTTP_HEAD) {
            char len_buf[32];
            snprintf(len_buf, sizeof(len_buf), "%zu", file->size);
            cwist_http_header_add(&res->headers, "Content-Length", len_buf);
            cwist_http_header_add(&res->headers, "Content-Type", mime);
            cwist_sstring_assign(res->body, "");
        } else if (file->data && file->node) {
            // ZERO COPY
            ttak_mem_node_acquire(file->node);
            cwist_http_response_set_body_ptr_managed(res, file->data, file->size, cwist_static_release_body, file->node);
            
            char len_buf[32];
            snprintf(len_buf, sizeof(len_buf), "%zu", file->size);
            cwist_http_header_add(&res->headers, "Content-Length", len_buf);
            cwist_http_header_add(&res->headers, "Content-Type", mime);
        } else {
            res->status_code = CWIST_HTTP_INTERNAL_ERROR;
            cwist_sstring_assign(res->body, "Static buffer missing");
        }
        res->status_code = CWIST_HTTP_OK;
    } else {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Not Found");
    }
    pthread_mutex_unlock(&mem->lock);
}
#include <limits.h>
#include <errno.h>

/**
 * @brief Allocate and initialize the top-level CWIST application object.
 * @return Newly created application, or NULL when allocation fails.
 */
cwist_app *cwist_app_create(void) {
    cwist_app *app = (cwist_app *)cwist_alloc(sizeof(cwist_app));
    if (!app) return NULL;
    
    app->port = 8080;
    app->use_ssl = false;
    app->cert_path = NULL;
    app->key_path = NULL;
    app->router = cwist_route_table_create();
    if (!app->router) {
        cwist_free(app);
        return NULL;
    }
    app->middlewares = NULL;
    app->ssl_ctx = NULL;
    app->error_handler = NULL;
    app->static_dirs = NULL;
    app->db = NULL;
    app->db_path = NULL;
    app->nuke_enabled = false;
    app->max_mem_space = 0;
    app->mem_manager = NULL;
    app->bdr_ctx = cwist_bdr_create();
    
    return app;
}

/**
 * @brief Append a middleware callback to the application's execution chain.
 * @param app Application being configured.
 * @param mw Middleware callback to append.
 */
void cwist_app_use(cwist_app *app, cwist_middleware_func mw) {
    if (!app || !mw) return;
    cwist_middleware_node *node = cwist_alloc(sizeof(cwist_middleware_node));
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

/**
 * @brief Override the static file memory budget used by cwist_mem_init().
 * @param app Application being configured.
 * @param size Maximum bytes reserved for static payload caching.
 */
void cwist_app_set_max_memspace(cwist_app *app, size_t size) {
    if (app) app->max_mem_space = size;
}

/**
 * @brief Install a custom HTTP error callback.
 * @param app Application being configured.
 * @param handler Handler invoked for framework-generated errors such as 404 responses.
 */
void cwist_app_set_error_handler(cwist_app *app, cwist_error_handler_func handler) {
    if (app) app->error_handler = handler;
}

/**
 * @brief Configure the Big Dumb Reply cache thresholds for the application.
 * @param app Application whose BDR context should be tuned.
 * @param max_bytes Maximum total cache footprint.
 * @param max_entry_age_sec Maximum age for cached entries.
 * @param revalidate_hits Hit count that forces revalidation.
 */
void cwist_app_configure_bdr(cwist_app *app, size_t max_bytes, time_t max_entry_age_sec, uint64_t revalidate_hits) {
    if (!app || !app->bdr_ctx) return;
    cwist_bdr_set_limits(app->bdr_ctx, max_bytes, max_entry_age_sec, revalidate_hits);
}

/**
 * @brief Release every resource owned by the application object.
 * @param app Application instance to destroy.
 */
void cwist_app_destroy(cwist_app *app) {
    if (!app) return;
    if (app->cert_path) cwist_free(app->cert_path);
    if (app->key_path) cwist_free(app->key_path);
    if (app->ssl_ctx) cwist_https_destroy_context(app->ssl_ctx);

    cwist_route_table_destroy(app->router);

    cwist_middleware_node *curr_m = app->middlewares;
    while (curr_m) {
        cwist_middleware_node *next = curr_m->next;
        cwist_free(curr_m);
        curr_m = next;
    }

    cwist_static_dir *curr_s = app->static_dirs;
    while (curr_s) {
        cwist_static_dir *next = curr_s->next;
        cwist_free(curr_s->url_prefix);
        cwist_free(curr_s->fs_root);
        cwist_free(curr_s);
        curr_s = next;
    }

    if (app->mem_manager) {
        app->mem_manager->watcher_running = false;
        // If thread was started, join it. 
        // Note: In simple destroy we might not have started it or might be crashing, but try join.
        if (app->mem_manager->watcher_thread) {
             pthread_join(app->mem_manager->watcher_thread, NULL);
        }
        pthread_mutex_destroy(&app->mem_manager->lock);
        
        for (size_t i = 0; i < app->mem_manager->file_count; i++) {
            cwist_free(app->mem_manager->files[i].path);
            cwist_free(app->mem_manager->files[i].fs_path);
        }
        for (size_t i = 0; i < app->mem_manager->file_count; i++) {
            if (app->mem_manager->files[i].node) {
                ttak_mem_tree_remove(&app->mem_manager->file_tree, app->mem_manager->files[i].node);
                app->mem_manager->files[i].node = NULL;
            }
        }
        cwist_free(app->mem_manager->files);
        ttak_mem_tree_destroy(&app->mem_manager->file_tree);
        cwist_free(app->mem_manager);
    }
    
    if (app->bdr_ctx) {
        cwist_bdr_destroy(app->bdr_ctx);
    }

    if (app->nuke_enabled) {
        cwist_nuke_close();
    }

    if (app->db) {
        // If nuke was used, app->db->conn was shared with nuke.
        // But nuke_close already closed its handles.
        // However, cwist_db_close will try to close it again if we are not careful.
        // Actually, NukeDB is a global singleton currently, so it's a bit messy.
        // If nuke_enabled is true, we should probably NOT call cwist_db_close(app->db)
        // OR we should make sure it's safe.
        // Based on cwist_db_close implementation, it calls sqlite3_close.
        if (app->nuke_enabled) {
             // Just free the wrapper, don't close the conn as Nuke already did it.
             ttak_mem_free(app->db);
        } else {
             cwist_db_close(app->db);
        }
        app->db = NULL;
    }
    if (app->db_path) {
        cwist_free(app->db_path);
    }
    
    cwist_free(app);
}

/**
 * @brief Advance the middleware chain or invoke the final route handler.
 * @param req Active request object.
 * @param res Active response object.
 */
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

/**
 * @brief Execute the application's middleware list around one final route handler.
 * @param app Application whose middleware chain should run.
 * @param req Active request object.
 * @param res Active response object.
 * @param final_handler Route handler to invoke after middleware.
 * @param handler_data Reserved handler payload slot.
 */
static void execute_chain(cwist_app *app, cwist_http_request *req, cwist_http_response *res, cwist_handler_func final_handler, void *handler_data) {
    mw_executor_ctx ctx = { app->middlewares, final_handler, handler_data };
    req->private_data = &ctx;
    mw_next_wrapper(req, res);
    req->private_data = NULL;
}

/**
 * @brief Enable TLS for the application using the supplied certificate pair.
 * @param app Application being configured.
 * @param cert_path PEM certificate chain path.
 * @param key_path PEM private key path.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_app_use_https(cwist_app *app, const char *cert_path, const char *key_path) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!app || !cert_path || !key_path) {
        err.error.err_i16 = -1;
        return err;
    }

    app->use_ssl = true;
    app->cert_path = cwist_strdup(cert_path);
    app->key_path = cwist_strdup(key_path);

    return cwist_https_init_context(&app->ssl_ctx, cert_path, key_path);
}

/**
 * @brief Open a SQLite database and attach it as the shared application handle.
 * @param app Application being configured.
 * @param db_path Filesystem path or SQLite URI to open.
 * @return Tagged CWIST error describing success or failure.
 */
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
        cwist_free(app->db_path);
    }

    app->db = db;
    app->db_path = cwist_strdup(db_path);
    app->nuke_enabled = false;
    return err;
}

/**
 * @brief Enable NUKE DB and fall back to standard SQLite when RAM bootstrap fails.
 * @param app Application being configured.
 * @param db_path On-disk database file to mirror into memory.
 * @param sync_interval_ms Synchronization interval forwarded to NUKE DB.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_app_use_nuke_db(cwist_app *app, const char *db_path, int sync_interval_ms) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!app || !db_path) {
        err.error.err_i16 = -1;
        return err;
    }

    int nuke_rc = cwist_nuke_init(db_path, sync_interval_ms);
    if (nuke_rc == CWIST_NUKE_ERR_LOW_MEMORY) {
        fprintf(stderr, "[CWIST] Nuke DB disabled for '%s' (insufficient RAM). Falling back to standard SQLite.\n", db_path);
        return cwist_app_use_db(app, db_path);
    } else if (nuke_rc != CWIST_NUKE_OK) {
        err.error.err_i16 = -1;
        return err;
    }

    if (app->db) {
        if (app->nuke_enabled) ttak_mem_free(app->db);
        else cwist_db_close(app->db);
        app->db = NULL;
    }
    if (app->db_path) {
        cwist_free(app->db_path);
    }

    app->db = (cwist_db *)ttak_mem_alloc_safe(sizeof(cwist_db),
                                              __TTAK_UNSAFE_MEM_FOREVER__,
                                              cwist_mem_now(),
                                              false,
                                              false,
                                              true,
                                              true,
                                              TTAK_MEM_DEFAULT);
    if (!app->db) {
        cwist_nuke_close();
        err.error.err_i16 = -1;
        return err;
    }
    app->db->conn = cwist_nuke_get_db();
    app->db_path = cwist_strdup(db_path);
    app->nuke_enabled = true;

    err.error.err_i16 = 0;
    return err;
}

/**
 * @brief Return the active shared database wrapper for the application.
 * @param app Application whose database handle should be queried.
 * @return Database wrapper, or NULL when no database is configured.
 */
cwist_db *cwist_app_get_db(cwist_app *app) {
    if (!app) return NULL;
    if (app->nuke_enabled) {
        app->db->conn = cwist_nuke_get_db();
    }
    return app->db;
}

/**
 * @brief Register a filesystem directory to be served beneath a URL prefix.
 * @param app Application being configured.
 * @param url_prefix Request-path prefix such as "/static".
 * @param directory Filesystem directory that backs the mapping.
 * @return Tagged CWIST error describing success or failure.
 */
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
        cwist_free(normalized);
        err.error.err_i16 = -1;
        return err;
    }

    cwist_static_dir *entry = (cwist_static_dir *)cwist_alloc(sizeof(cwist_static_dir));
    if (!entry) {
        cwist_free(normalized);
        cwist_free(resolved);
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

static void add_route(cwist_app *app,
                      const char *path,
                      cwist_http_method_t method,
                      cwist_handler_func handler,
                      cwist_endpoint_opt_t opts) {
    if (!app || !app->router || !path) return;
    if (opts == 0) {
        opts = CWIST_ENDPOINT_DEFAULT;
    }
    cwist_route_table_insert(app->router, path, method, handler, NULL, opts);
}

/**
 * @brief Register a GET handler with default endpoint options.
 * @param app Application being configured.
 * @param path Exact route path.
 * @param handler HTTP handler invoked for matching requests.
 */
void cwist_app_get(cwist_app *app, const char *path, cwist_handler_func handler) {
    add_route(app, path, CWIST_HTTP_GET, handler, CWIST_ENDPOINT_DEFAULT);
}

/**
 * @brief Register a POST handler with default endpoint options.
 * @param app Application being configured.
 * @param path Exact route path.
 * @param handler HTTP handler invoked for matching requests.
 */
void cwist_app_post(cwist_app *app, const char *path, cwist_handler_func handler) {
    add_route(app, path, CWIST_HTTP_POST, handler, CWIST_ENDPOINT_DEFAULT);
}

/**
 * @brief Register a WebSocket upgrade endpoint with default options.
 * @param app Application being configured.
 * @param path Exact GET route that should upgrade to WebSocket.
 * @param handler WebSocket handler invoked after a successful upgrade.
 */
void cwist_app_ws(cwist_app *app, const char *path, cwist_ws_handler_func handler) {
    if (!app || !app->router || !path) return;
    cwist_route_table_insert(app->router, path, CWIST_HTTP_GET, NULL, handler, CWIST_ENDPOINT_DEFAULT);
}

/**
 * @brief Register a GET handler with explicit endpoint options.
 * @param app Application being configured.
 * @param path Exact route path.
 * @param handler HTTP handler invoked for matching requests.
 * @param opts Endpoint flags controlling cache and transport behavior.
 */
void cwist_app_get_opt(cwist_app *app, const char *path, cwist_handler_func handler, cwist_endpoint_opt_t opts) {
    add_route(app, path, CWIST_HTTP_GET, handler, opts);
}

/**
 * @brief Register a POST handler with explicit endpoint options.
 * @param app Application being configured.
 * @param path Exact route path.
 * @param handler HTTP handler invoked for matching requests.
 * @param opts Endpoint flags controlling cache and transport behavior.
 */
void cwist_app_post_opt(cwist_app *app, const char *path, cwist_handler_func handler, cwist_endpoint_opt_t opts) {
    add_route(app, path, CWIST_HTTP_POST, handler, opts);
}

/**
 * @brief Register a WebSocket route with explicit endpoint options.
 * @param app Application being configured.
 * @param path Exact GET route that should upgrade to WebSocket.
 * @param handler WebSocket handler invoked after a successful upgrade.
 * @param opts Endpoint flags associated with the route.
 */
void cwist_app_ws_opt(cwist_app *app, const char *path, cwist_ws_handler_func handler, cwist_endpoint_opt_t opts) {
    if (!app || !app->router || !path) return;
    if (opts == 0) {
        opts = CWIST_ENDPOINT_DEFAULT;
    }
    cwist_route_table_insert(app->router, path, CWIST_HTTP_GET, NULL, handler, opts);
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

    req->endpoint_opts = CWIST_ENDPOINT_DEFAULT;
    if (res) {
        res->endpoint_opts = req->endpoint_opts;
    }

    cwist_static_request_info static_info = {0};
    if (cwist_prepare_static(app, req, &static_info)) {
        req->endpoint_opts = CWIST_ENDPOINT_FILE;
        if (res) res->endpoint_opts = req->endpoint_opts;
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
        req->endpoint_opts = found_route->opts ? found_route->opts : CWIST_ENDPOINT_DEFAULT;
        if (res) res->endpoint_opts = req->endpoint_opts;
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
    char *read_buf = cwist_alloc(CWIST_HTTP_READ_BUFFER_SIZE);
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

        // --- Big Dumb Reply (Read) ---
        if (app->bdr_ctx && req->method == CWIST_HTTP_GET) {
            size_t cached_len = 0;
            const void *cached_blob = cwist_bdr_get(app->bdr_ctx, "GET", req->path->data, &cached_len);
            if (cached_blob) {
                // BDR Hit! Blast it out.
                send(client_fd, cached_blob, cached_len, 0); // Flags handled by socket opt ideally or just 0
                
                // Cleanup and Loop
                bool keep_alive = req->keep_alive;
                cwist_http_request_destroy(req);
                if (!keep_alive) break;
                continue;
            }
        }
        // -----------------------------

        cwist_http_response *res = cwist_http_response_create();
        if (!res) {
            cwist_http_request_destroy(req);
            break;
        }
        
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        internal_route_handler(app, req, res);
        
        clock_gettime(CLOCK_MONOTONIC, &end);
        uint64_t duration_ms = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000;

        bool keep_alive = req->keep_alive && res->keep_alive;
        bool upgraded = req->upgraded;
        
        if (!req->upgraded) {
            if (cwist_http_send_response(client_fd, res).error.err_i16 < 0) {
                cwist_http_response_destroy(res);
                cwist_http_request_destroy(req);
                break;
            }
            
            // --- Big Dumb Reply (Learn) ---
            bool endpoint_fixed = cwist_endpoint_has(req->endpoint_opts, CWIST_ENDPOINT_FIXED);
            bool endpoint_file = cwist_endpoint_has(req->endpoint_opts, CWIST_ENDPOINT_FILE);
            if (app->bdr_ctx &&
                req->method == CWIST_HTTP_GET &&
                !endpoint_file &&
                (endpoint_fixed || duration_ms > (uint64_t)app->bdr_ctx->latency_threshold_ms)) {
                // Too slow! Cache it.
                // We need to serialize the response we just sent.
                // Note: This duplicates serialization work (once in send_response, once here).
                // Optimization: send_response could return the blob, or we serialize first then send.
                // For now, re-serialize for BDR.
                cwist_sstring *serialized = cwist_http_stringify_response(res);
                if (serialized) {
                     cwist_bdr_put(app->bdr_ctx, "GET", req->path->data, serialized->data, serialized->size);
                     cwist_sstring_destroy(serialized);
                }
            }
            // ------------------------------
        }
        
        cwist_http_response_destroy(res);
        cwist_http_request_destroy(req);
        
        if (!keep_alive || upgraded) {
            break;
        }
    }
    
    cwist_free(read_buf);
    close(client_fd);
}

/**
 * @brief Initialize runtime services and enter the HTTP or HTTPS server loop.
 * @param app Application instance to run.
 * @param port TCP port to bind.
 * @return 0 on success, or -1 when initialization or bind fails.
 */
int cwist_app_listen(cwist_app *app, int port) {
    if (!app) return -1;
    app->port = port;
    
    // Initialize Memory Manager
    cwist_mem_init(app);
    if (app->mem_manager) {
        app->mem_manager->watcher_running = true;
        pthread_create(&app->mem_manager->watcher_thread, NULL, cwist_mem_watcher, app);
    }
    
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
