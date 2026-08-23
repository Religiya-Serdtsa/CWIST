#if defined(__APPLE__)
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#elif !defined(__FreeBSD__) && !defined(__NetBSD__) && !defined(__OpenBSD__) && !defined(__DragonFly__)
#define _POSIX_C_SOURCE 200809L
#endif
#include <cwist/net/http/http.h>
#include <cwist/net/http/session.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/sys/err/cwist_err.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/arena.h>
#include <cwist/sys/app/shutdown.h>
#include <cwist/sys/io/reactor.h>
#include <cwist/core/log.h>
#include <cwist/sys/metrics/metrics.h>
#include <ttak/mols_control.h>
#include <ttak/net/lattice.h>
#include <ttak/priority/scheduler.h>
#include "simd_parser.h"

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
#include <stdatomic.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef __linux__
#include <sys/epoll.h>
#endif
#if defined(__linux__)
#include <sys/sendfile.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/uio.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/event.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
    #include <windows.h>
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    #include <sys/types.h>
    #include <sys/sysctl.h>
#else
    #include <unistd.h>
#endif

long get_cpu_cores(void) {
#if defined(_WIN32) || defined(_WIN64)
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return (long)sysinfo.dwNumberOfProcessors;
#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    if (sched_getaffinity(0, sizeof(cpuset), &cpuset) == 0) {
        int count = CPU_COUNT(&cpuset);
        if (count > 0) return (long)count;
    }
#if defined(_SC_NPROCESSORS_ONLN)
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc > 0) return nproc;
#endif
    return 1;
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    int mib[2];
    int nproc = 0;
    size_t len = sizeof(nproc);
    mib[0] = CTL_HW;
#if defined(HW_NCPUONLINE)
    mib[1] = HW_NCPUONLINE;
#else
    mib[1] = HW_NCPU;
#endif
    if (sysctl(mib, 2, &nproc, &len, NULL, 0) == 0) {
        return (long)nproc;
    }
    return 4;
#elif defined(_SC_NPROCESSORS_ONLN)
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc > 0) return nproc;
    return 1;
#else
    return 1;
#endif
}

static unsigned int g_http_pool_core_limit = 0;

void cwist_http_pool_limit_core(unsigned int limit) {
    g_http_pool_core_limit = limit;
}

long get_optimal_thread_count(void) {
    if (g_http_pool_core_limit > 0) {
        return (long)g_http_pool_core_limit;
    }
    const char *env = getenv("CWIST_WORKER_THREADS");
    if (env && env[0]) {
        long override = atol(env);
        if (override > 0) return override;
    }

    long cores = get_cpu_cores();
    if (cores < 1) cores = 1;

    long workers = cores;
    const char *w_env = getenv("CWIST_WORKERS");
    if (w_env && w_env[0]) {
        if (strcmp(w_env, "auto") != 0) {
            long parsed = atol(w_env);
            if (parsed > 0) workers = parsed;
        }
    }

    if (workers == 1) {
        /* Keep-alive handlers park on their connection, so the pool must
         * cover many more concurrent connections than there are cores.
         * Blocked threads are nearly free (futex sleep); undersizing the
         * pool caps throughput at threads x per-conn rate. */
        long count = cores * 8;
        if (count < 32) count = 32;
        if (count > 256) count = 256;
        return count;
    }

    /* Dynamic thread downscaling: distribute thread budget proportionally across forked worker processes */
    long threads_per_worker = (cores * 8) / workers;
    if (threads_per_worker < 8) threads_per_worker = 8;
    if (threads_per_worker > 64) threads_per_worker = 64;
    return threads_per_worker;
}

#define HTTP_TASKS_PER_THREAD 32768

typedef struct {
    pthread_t thread;
    cwist_reactor_t *reactor;
    uint32_t worker_id;
} http_thread_worker_t;

static size_t g_rr_index = 0;
static long g_http_thread_count;
static http_thread_worker_t *g_workers = NULL;

/* Saturation backpressure: track in-flight connections and shed load once
 * every worker thread could be parked on a connection many times over.
 * Past the limit there is no throughput left to win - new arrivals would
 * only inflate tail latency for traffic already being served. Shedding is
 * a single fixed 503 write + close, so it adds no latency to others. */
static _Atomic long g_http_inflight = 0;
#define CWIST_HTTP_INFLIGHT_PER_THREAD 32

static const char CWIST_HTTP_503[] =
    "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

typedef struct {
    int client_fd;
    void (*handler_func)(int, void *);
    void *ctx;
    cwist_reactor_t *reactor;
} http_conn_ctx_t;

/* The reactor copies this struct into its pooled, zeroed slot at add time,
 * so the callback reads the slot's inline storage - no per-connection
 * heap allocation, nothing to free here. */
static void http_conn_event_cb(int fd, void *ctx) {
    http_conn_ctx_t *c = (http_conn_ctx_t *)ctx;
    void (*handler)(int, void *) = c->handler_func;
    void *user_ctx = c->ctx;

    handler(fd, user_ctx);
    atomic_fetch_sub_explicit(&g_http_inflight, 1, memory_order_release);
}

static _Thread_local http_thread_worker_t *t_current_worker = NULL;

static void *http_pool_worker(void *arg) {
    http_thread_worker_t *w = (http_thread_worker_t *)arg;
    t_current_worker = w;
    ttak_net_lattice_set_worker_id(w->worker_id);

    cwist_reactor_run(w->reactor);
    return NULL;
}

int cwist_http_pool_init(void) {
    g_http_thread_count = get_optimal_thread_count();
    g_workers = cwist_alloc(g_http_thread_count * sizeof(http_thread_worker_t));
    g_rr_index = 0;
    memset(g_workers, 0, g_http_thread_count * sizeof(http_thread_worker_t));

    for (int i = 0; i < g_http_thread_count; i++) {
        g_workers[i].reactor = cwist_reactor_create();
        if (!g_workers[i].reactor) return -1;
        g_workers[i].worker_id = (uint32_t)i;
        if (pthread_create(&g_workers[i].thread, NULL, http_pool_worker, &g_workers[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

void cwist_http_pool_submit(int client_fd, void (*handler)(int, void *), void *ctx) {
    /* Backpressure gate: shed before doing any routing/allocation work when
     * saturated. In-flight = queued + actively served connections. */
    long limit = g_http_thread_count * CWIST_HTTP_INFLIGHT_PER_THREAD;
    long inflight = atomic_fetch_add_explicit(&g_http_inflight, 1, memory_order_acq_rel) + 1;
    if (inflight > limit) {
        atomic_fetch_sub_explicit(&g_http_inflight, 1, memory_order_release);
        send(client_fd, CWIST_HTTP_503, sizeof(CWIST_HTTP_503) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
        close(client_fd);
        return;
    }

    /* Deterministic worker selection using Choi Seok-jeong's MOLS to minimize cache bouncing. */
    uint16_t node_id = (uint16_t)(client_fd % TTAK_MOLS_NODE_COUNT);
    uint32_t mixed = ttak_apply_mols_control(node_id, (uint32_t)g_rr_index);
    size_t worker_idx = mixed % (size_t)g_http_thread_count;

    g_rr_index = (g_rr_index + 1) % (size_t)g_http_thread_count;

    http_thread_worker_t *w = &g_workers[worker_idx];

    http_conn_ctx_t c = {
        .client_fd = client_fd,
        .handler_func = handler,
        .ctx = ctx,
        .reactor = w->reactor,
    };

    if (!cwist_reactor_add(w->reactor, client_fd, http_conn_event_cb, &c, sizeof(c))) {
        atomic_fetch_sub_explicit(&g_http_inflight, 1, memory_order_release);
        close(client_fd);
    }
}

bool cwist_http_pool_rearm_current(int client_fd, void (*handler)(int, void *), void *ctx) {
    if (!t_current_worker || !t_current_worker->reactor || client_fd < 0) return false;

    http_conn_ctx_t c = {
        .client_fd = client_fd,
        .handler_func = handler,
        .ctx = ctx,
        .reactor = t_current_worker->reactor,
    };

    return cwist_reactor_add(t_current_worker->reactor, client_fd, http_conn_event_cb, &c, sizeof(c));
}

void cwist_http_pool_destroy(void) {
    if (!g_workers) return;
    for (int i = 0; i < g_http_thread_count; i++) {
        if (g_workers[i].reactor) {
            cwist_reactor_stop(g_workers[i].reactor);
        }
    }
    for (int i = 0; i < g_http_thread_count; i++) {
        pthread_join(g_workers[i].thread, NULL);
        if (g_workers[i].reactor) {
            cwist_reactor_destroy(g_workers[i].reactor);
            g_workers[i].reactor = NULL;
        }
    }
    cwist_free(g_workers);
    g_workers = NULL;
    g_http_thread_count = 0;
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

#define MAKE_MAGIC4(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define MAGIC_GET  MAKE_MAGIC4('G', 'E', 'T', ' ')
#define MAGIC_POST MAKE_MAGIC4('P', 'O', 'S', 'T')
#define MAGIC_PUT  MAKE_MAGIC4('P', 'U', 'T', ' ')
#define MAGIC_DELE MAKE_MAGIC4('D', 'E', 'L', 'E')
#define MAGIC_HEAD MAKE_MAGIC4('H', 'E', 'A', 'D')

cwist_http_method_t cwist_http_string_to_method_len(const char *str, size_t len) {
    if (!str || len == 0) return CWIST_HTTP_UNKNOWN;
    
    /* Ultra-fast path: SWAR 4-byte magic lookup for GET, POST, PUT, DELETE, HEAD */
    if (len >= 3) {
        uint32_t m = 0;
        memcpy(&m, str, sizeof(uint32_t));
        switch (m) {
            case MAGIC_GET:  return CWIST_HTTP_GET;
            case MAGIC_POST: return CWIST_HTTP_POST;
            case MAGIC_PUT:  return CWIST_HTTP_PUT;
            case MAGIC_DELE: if (len >= 6 && memcmp(str, "DELETE", 6) == 0) return CWIST_HTTP_DELETE; break;
            case MAGIC_HEAD: if (len == 4) return CWIST_HTTP_HEAD; break;
            default: break;
        }
    }

    if (len == 3) {
        uint32_t v = 0;
        memcpy(&v, str, 3);
        if ((v & 0x00FFFFFF) == 0x00544547) return CWIST_HTTP_GET;
        if ((v & 0x00FFFFFF) == 0x00545550) return CWIST_HTTP_PUT;
    } else if (len == 4) {
        uint32_t v = 0;
        memcpy(&v, str, 4);
        if (v == 0x54534F50) return CWIST_HTTP_POST;
        if (v == 0x44414548) return CWIST_HTTP_HEAD;
    } else if (len == 5) {
        if (memcmp(str, "PATCH", 5) == 0) return CWIST_HTTP_PATCH;
    } else if (len == 6) {
        if (memcmp(str, "DELETE", 6) == 0) return CWIST_HTTP_DELETE;
    } else if (len == 7) {
        if (memcmp(str, "OPTIONS", 7) == 0) return CWIST_HTTP_OPTIONS;
    }
    return CWIST_HTTP_UNKNOWN;
}

cwist_http_method_t cwist_http_string_to_method(const char *method_str) {
    if (!method_str) return CWIST_HTTP_UNKNOWN;
    return cwist_http_string_to_method_len(method_str, strlen(method_str));
}

/* --- Header Manipulation --- */

/**
 * @brief Allocate a fixed-size struct from an arena, falling back to the heap.
 * @param arena Request/response arena, or NULL for plain heap allocation.
 * @param size Struct size in bytes.
 * @return Zeroed memory block, or NULL when both paths fail.
 */
static void *cwist_http_struct_alloc(cwist_arena_t *arena, size_t size) {
    void *p = arena ? cwist_arena_alloc(arena, size) : NULL;
    if (p) {
        memset(p, 0, size);
        return p;
    }
    return cwist_alloc(size);
}

/**
 * @brief Create an sstring whose struct lives in an arena when possible.
 *
 * The character buffer still comes from the heap (cwist_realloc); only the
 * fixed-size cwist_sstring struct is bump-allocated. Arena-owned structs get
 * owns_storage = false so cwist_sstring_destroy releases the buffer but
 * leaves the struct to the arena.
 */
static cwist_sstring *cwist_http_sstring_create(cwist_arena_t *arena) {
    bool from_arena = false;
    cwist_sstring *str = NULL;
    if (arena) {
        str = (cwist_sstring *)cwist_arena_alloc(arena, sizeof(cwist_sstring));
        if (str) from_arena = true;
    }
    if (!str) {
        str = (cwist_sstring *)cwist_alloc(sizeof(cwist_sstring));
    }
    if (!str) return NULL;

    memset(str, 0, sizeof(cwist_sstring));
    str->is_fixed = false;
    str->owns_storage = !from_arena;
    str->size = 0;
    str->data = NULL;
    str->get_size = cwist_sstring_get_size;
    str->compare = cwist_sstring_compare_sstring;
    str->copy = cwist_sstring_copy_sstring;
    str->append = cwist_sstring_append_sstring;

    return str;
}

/**
 * @brief Assign into an sstring, carving the character buffer from an arena.
 *
 * Arena-backed buffers are marked as borrowed so sstring teardown never
 * frees them individually (the arena releases everything in one shot) and a
 * later mutation transparently detaches to the heap. Falls back to a regular
 * heap assign when the arena is NULL or exhausted.
 */
static cwist_error_t cwist_http_sstring_assign_arena(cwist_sstring *str, cwist_arena_t *arena, const char *data, size_t len) {
    if (arena) {
        char *buf = (char *)cwist_arena_alloc(arena, len + 1);
        if (buf) {
            if (str->data && !str->borrows_buffer) {
                cwist_free(str->data);
            }
            if (data && len > 0) memcpy(buf, data, len);
            buf[len] = '\0';
            str->data = buf;
            str->size = len;
            str->borrows_buffer = true;
            cwist_error_t err = make_error(CWIST_ERR_INT8);
            err.error.err_i8 = ERR_SSTRING_OKAY;
            return err;
        }
    }
    return cwist_sstring_assign_len(str, data, len);
}

/**
 * @brief Prepend one header node with length, optionally bump-allocated from an arena.
 */
static cwist_error_t cwist_http_header_add_ex_len(cwist_http_header_node **head, cwist_arena_t *arena, const char *key, size_t key_len, const char *value, size_t value_len) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    bool from_arena = false;
    cwist_http_header_node *node = NULL;
    if (arena) {
        node = (cwist_http_header_node *)cwist_arena_alloc(arena, sizeof(cwist_http_header_node));
        if (node) {
            memset(node, 0, sizeof(cwist_http_header_node));
            from_arena = true;
        }
    }
    if (!node) {
        node = (cwist_http_header_node *)cwist_alloc(sizeof(cwist_http_header_node));
    }
    if (!node) {
        err = make_error(CWIST_ERR_JSON);
        err.error.err_json = cJSON_CreateObject();
        cJSON_AddStringToObject(err.error.err_json, "http_error", "Failed to allocate header");
        return err;
    }
    node->arena_owned = from_arena;

    node->key = cwist_http_sstring_create(arena);
    node->value = cwist_http_sstring_create(arena);
    node->next = NULL;

    cwist_http_sstring_assign_arena(node->key, arena, key, key_len);
    cwist_http_sstring_assign_arena(node->value, arena, value, value_len);

    node->next = *head;
    *head = node;

    err.error.err_i16 = 0; // Success
    return err;
}

/**
 * @brief Prepend one header node, optionally bump-allocated from an arena.
 * @param head Header-list head pointer to update.
 * @param arena Arena to carve the node from, or NULL for heap allocation.
 * @param key Header name to store.
 * @param value Header value to store.
 * @return Tagged CWIST error describing success or allocation failure.
 */
static cwist_error_t cwist_http_header_add_ex(cwist_http_header_node **head, cwist_arena_t *arena, const char *key, const char *value) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    bool from_arena = false;
    cwist_http_header_node *node = NULL;
    if (arena) {
        node = (cwist_http_header_node *)cwist_arena_alloc(arena, sizeof(cwist_http_header_node));
        if (node) {
            memset(node, 0, sizeof(cwist_http_header_node));
            from_arena = true;
        }
    }
    if (!node) {
        node = (cwist_http_header_node *)cwist_alloc(sizeof(cwist_http_header_node));
    }
    if (!node) {
        err = make_error(CWIST_ERR_JSON);
        err.error.err_json = cJSON_CreateObject();
        cJSON_AddStringToObject(err.error.err_json, "http_error", "Failed to allocate header");
        return err;
    }
    node->arena_owned = from_arena;

    node->key = cwist_http_sstring_create(arena);
    node->value = cwist_http_sstring_create(arena);
    node->next = NULL;

    cwist_http_sstring_assign_arena(node->key, arena, key, key ? strlen(key) : 0);
    cwist_http_sstring_assign_arena(node->value, arena, value, value ? strlen(value) : 0);

    node->next = *head;
    *head = node;

    err.error.err_i16 = 0; // Success
    return err;
}

/**
 * @brief Prepend a header whose key/value live in static storage (zero-copy).
 *
 * Used for compile-time constant headers such as the default security set:
 * the node and sstring structs come from the arena and the character bytes
 * are borrowed, so a default response pays no heap traffic for headers at
 * all. Key and value must outlive the header list.
 */
static cwist_error_t cwist_http_header_add_static(cwist_http_header_node **head, cwist_arena_t *arena, const char *key, const char *value) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    bool from_arena = false;
    cwist_http_header_node *node = NULL;
    if (arena) {
        node = (cwist_http_header_node *)cwist_arena_alloc(arena, sizeof(cwist_http_header_node));
        if (node) {
            memset(node, 0, sizeof(cwist_http_header_node));
            from_arena = true;
        }
    }
    if (!node) {
        node = (cwist_http_header_node *)cwist_alloc(sizeof(cwist_http_header_node));
    }
    if (!node) {
        err = make_error(CWIST_ERR_JSON);
        err.error.err_json = cJSON_CreateObject();
        cJSON_AddStringToObject(err.error.err_json, "http_error", "Failed to allocate header");
        return err;
    }
    node->arena_owned = from_arena;

    node->key = cwist_http_sstring_create(arena);
    node->value = cwist_http_sstring_create(arena);
    node->next = NULL;

    if (!node->key || !node->value) {
        if (!from_arena) cwist_free(node);
        err = make_error(CWIST_ERR_JSON);
        err.error.err_json = cJSON_CreateObject();
        cJSON_AddStringToObject(err.error.err_json, "http_error", "Failed to allocate header strings");
        return err;
    }

    cwist_sstring_borrow(node->key, key, strlen(key));
    cwist_sstring_borrow(node->value, value, strlen(value));

    node->next = *head;
    *head = node;

    err.error.err_i16 = 0; // Success
    return err;
}

/**
 * @brief Prepend one header node to the linked-list header collection.
 * @param head Header-list head pointer to update.
 * @param key Header name to store.
 * @param value Header value to store.
 * @return Tagged CWIST error describing success or allocation failure.
 */
cwist_error_t cwist_http_header_add(cwist_http_header_node **head, const char *key, const char *value) {
    return cwist_http_header_add_ex(head, NULL, key, value);
}

/**
 * @brief Find a header value using case-insensitive header-name comparison.
 * @param head Head of the header linked list.
 * @param key Header name to search for.
 * @return Raw header value string, or NULL when absent.
 */
char *cwist_http_header_get(cwist_http_header_node *head, const char *key) {
    if (!head || !key) return NULL;
    size_t klen = strlen(key);
    cwist_http_header_node *curr = head;
    while (curr) {
        if (curr->key && curr->key->data && curr->key->size == klen) {
            if (strcasecmp(curr->key->data, key) == 0) {
                return curr->value ? curr->value->data : NULL;
            }
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
    cwist_arena_t *arena = (cwist_arena_t *)res->arena;

    /* All key/value pairs are compile-time constants: borrow them instead of
     * heap-copying, so a default response performs zero heap allocations for
     * its security headers (arena carve only). */
    if (!cwist_http_header_get(res->headers, "X-Frame-Options")) {
        cwist_http_header_add_static(&res->headers, arena, "X-Frame-Options", "DENY");
    }
    if (!cwist_http_header_get(res->headers, "X-Content-Type-Options")) {
        cwist_http_header_add_static(&res->headers, arena, "X-Content-Type-Options", "nosniff");
    }
    if (!cwist_http_header_get(res->headers, "Referrer-Policy")) {
        cwist_http_header_add_static(&res->headers, arena, "Referrer-Policy", "strict-origin-when-cross-origin");
    }
    if (!cwist_http_header_get(res->headers, "Content-Security-Policy")) {
        cwist_http_header_add_static(&res->headers, arena, "Content-Security-Policy",
            "default-src 'self'; "
            "script-src 'self' https://cdnjs.cloudflare.com; "
            "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com https://cdn.jsdelivr.net https://cdnjs.cloudflare.com; "
            "font-src 'self' https://fonts.gstatic.com https://cdn.jsdelivr.net; "
            "img-src 'self'; "
            "connect-src 'self'; "
            "frame-ancestors 'none'; "
            "base-uri 'self'; "
            "form-action 'self'; "
            "object-src 'none';");
    }
    if (!cwist_http_header_get(res->headers, "Cross-Origin-Resource-Policy")) {
        cwist_http_header_add_static(&res->headers, arena, "Cross-Origin-Resource-Policy", "same-origin");
    }
    if (!cwist_http_header_get(res->headers, "Strict-Transport-Security")) {
        cwist_http_header_add_static(&res->headers, arena, "Strict-Transport-Security", "max-age=31536000; includeSubDomains");
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
        if (!curr->arena_owned) {
            cwist_free(curr);
        }
        curr = next;
    }
}

/**
 * @brief Copy the cached RFC 7231 IMF-fixdate string for the current second.
 * @param out_buf Destination buffer; receives 29 date bytes plus NUL.
 */
static void cwist_get_cached_date_header(char out_buf[36]) {
    static _Atomic time_t g_last_sec = 0;
    static char g_date_str[36] = {0};
    static pthread_mutex_t g_date_lock = PTHREAD_MUTEX_INITIALIZER;

    time_t now = time(NULL);
    time_t last = atomic_load_explicit(&g_last_sec, memory_order_relaxed);
    if (now != last) {
        pthread_mutex_lock(&g_date_lock);
        if (now != atomic_load_explicit(&g_last_sec, memory_order_relaxed)) {
            struct tm gmt;
#if defined(_WIN32)
            gmtime_s(&gmt, &now);
#else
            gmtime_r(&now, &gmt);
#endif
            strftime(g_date_str, sizeof(g_date_str), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
            atomic_store_explicit(&g_last_sec, now, memory_order_release);
        }
        pthread_mutex_unlock(&g_date_lock);
    }
    memcpy(out_buf, g_date_str, 30);
    out_buf[29] = '\0';
}

/* --- Request Lifecycle --- */

/**
 * @brief Allocate and initialize a default HTTP request object.
 * @return Newly allocated request, or NULL on allocation failure.
 */
cwist_http_request *cwist_http_request_create(void) {
    cwist_arena_t *arena = cwist_arena_create(0);
    cwist_http_request *req = (cwist_http_request *)cwist_http_struct_alloc(arena, sizeof(cwist_http_request));
    if (!req) {
        cwist_arena_destroy(arena);
        return NULL;
    }

    req->method = CWIST_HTTP_GET; // Default
    req->path = cwist_http_sstring_create(arena);
    req->query = cwist_http_sstring_create(arena);
    req->query_params = NULL;
    req->path_params = NULL;
    req->version = cwist_http_sstring_create(arena);
    req->headers = NULL;
    req->body = cwist_http_sstring_create(arena);
    req->keep_alive = true;
    req->client_fd = -1;
    req->app = NULL;
    req->db = NULL;
    req->flash = NULL;
    req->upgraded = false;
    req->content_length = 0;
    req->stream_id = 0;
    req->private_data = NULL;
    req->endpoint_opts = CWIST_ENDPOINT_DEFAULT;
    req->arena = arena;

    // Defaults (borrowed statics; parsing overwrites them via arena/heap assign)
    cwist_sstring_borrow(req->version, "HTTP/1.1", 8);
    cwist_sstring_borrow(req->path, "/", 1);

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
        cwist_arena_t *arena = (cwist_arena_t *)req->arena;
        cwist_sstring_destroy(req->path);
        cwist_sstring_destroy(req->query);
        cwist_query_map_destroy(req->query_params);
        cwist_query_map_destroy(req->path_params);
        cwist_sstring_destroy(req->version);
        cwist_sstring_destroy(req->body);
        cwist_query_map_destroy(req->flash);
        cwist_session_destroy(req->session);
        cwist_free(req->csrf_token);
        cwist_http_header_free_all(req->headers);
        if (!arena || !cwist_arena_owns(arena, req)) {
            cwist_free(req);
        }
        /* Releases every arena-carved struct (request, sstrings, header
         * nodes) in one shot; heap-owned buffers were freed above. */
        cwist_arena_destroy(arena);
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
    cwist_arena_t *arena = cwist_arena_create(0);
    cwist_http_response *res = (cwist_http_response *)cwist_http_struct_alloc(arena, sizeof(cwist_http_response));
    if (!res) {
        cwist_arena_destroy(arena);
        return NULL;
    }

    res->version = cwist_http_sstring_create(arena);
    res->status_code = CWIST_HTTP_OK;
    res->status_text = cwist_http_sstring_create(arena);
    res->headers = NULL;
    res->body = cwist_http_sstring_create(arena);
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
    res->arena = arena;

    // Defaults (borrowed statics; handlers may overwrite via regular assign)
    cwist_sstring_borrow(res->version, "HTTP/1.1", 8);
    cwist_sstring_borrow(res->status_text, "OK", 2);

    return res;
}

/**
 * @brief Destroy an HTTP response and release any attached body resources.
 * @param res Response object to destroy.
 */
void cwist_http_response_destroy(cwist_http_response *res) {
    if (res) {
        cwist_arena_t *arena = (cwist_arena_t *)res->arena;
        cwist_http_response_release_ptr_body(res);
        cwist_http_response_release_file_stream(res);
        cwist_sstring_destroy(res->version);
        cwist_sstring_destroy(res->status_text);
        cwist_sstring_destroy(res->body);
        cwist_http_header_free_all(res->headers);
        cwist_free(res->alt_svc);
        if (!arena || !cwist_arena_owns(arena, res)) {
            cwist_free(res);
        }
        cwist_arena_destroy(arena);
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

    /* Fast path for default HTTP/1.1 200 OK response with no custom headers */
    if (res->status_code == 200 && !res->headers && !res->alt_svc && buf_size >= 128) {
        /* Compact ring/line buffer optimization for high-throughput pipelining */
        static const char status_prefix[] = "HTTP/1.1 200 OK\r\nContent-Length: ";
        memcpy(buf, status_prefix, sizeof(status_prefix) - 1);
        size_t offset = sizeof(status_prefix) - 1;

        /* Fast integer to ascii without snprintf overhead */
        char num_buf[20];
        char *p = num_buf + sizeof(num_buf);
        size_t tmp_len = body_len;
        do {
            *--p = '0' + (tmp_len % 10);
            tmp_len /= 10;
        } while (tmp_len > 0);
        size_t num_len = (num_buf + sizeof(num_buf)) - p;
        memcpy(buf + offset, p, num_len);
        offset += num_len;

        if (res->keep_alive) {
            static const char conn_ka[] = "\r\nConnection: keep-alive\r\n\r\n";
            memcpy(buf + offset, conn_ka, sizeof(conn_ka) - 1);
            offset += sizeof(conn_ka) - 1;
        } else {
            static const char conn_cl[] = "\r\nConnection: close\r\n\r\n";
            memcpy(buf + offset, conn_cl, sizeof(conn_cl) - 1);
            offset += sizeof(conn_cl) - 1;
        }
        return offset;
    }

    size_t offset = 0;
    
    // Status Line
    const char *status_txt = (res->status_text && res->status_text->data) 
                             ? res->status_text->data 
                             : "OK";
    if (offset < buf_size) {
        int n = snprintf(buf + offset, buf_size - offset, "%s %d %s\r\n",
                 res->version->data ? res->version->data : "HTTP/1.1",
                 res->status_code,
                 status_txt);
        if (n > 0) {
            offset += n;
            if (offset > buf_size) offset = buf_size;
        }
    }

    // Headers: memcpy with known sstring lengths (no snprintf/strlen overhead),
    // detecting Date/Content-Length/Connection in the same single pass with a
    // length + first-byte filter before falling back to strcasecmp.
    bool have_date = false, have_clen = false, have_conn = false;
    cwist_http_header_node *curr = res->headers;
    while (curr) {
        if (curr->key->data && curr->value->data) {
            size_t klen = curr->key->size;
            size_t vlen = curr->value->size;
            if (offset + klen + 2 + vlen + 2 <= buf_size) {
                memcpy(buf + offset, curr->key->data, klen);
                buf[offset + klen] = ':';
                buf[offset + klen + 1] = ' ';
                memcpy(buf + offset + klen + 2, curr->value->data, vlen);
                buf[offset + klen + 2 + vlen] = '\r';
                buf[offset + klen + 2 + vlen + 1] = '\n';
                offset += klen + vlen + 4;
            }
            char k0 = curr->key->data[0];
            if (klen == 4 && (k0 == 'D' || k0 == 'd')) {
                if (strcasecmp(curr->key->data, "date") == 0) have_date = true;
            } else if (klen == 14 && (k0 == 'C' || k0 == 'c')) {
                if (strcasecmp(curr->key->data, "content-length") == 0) have_clen = true;
            } else if (klen == 10 && (k0 == 'C' || k0 == 'c')) {
                if (strcasecmp(curr->key->data, "connection") == 0) have_conn = true;
            }
        }
        curr = curr->next;
    }

    if (!have_date && offset + 37 <= buf_size) {
        char date_str[36];
        cwist_get_cached_date_header(date_str); /* 29 bytes, NUL-terminated */
        memcpy(buf + offset, "Date: ", 6);
        memcpy(buf + offset + 6, date_str, 29);
        buf[offset + 35] = '\r';
        buf[offset + 36] = '\n';
        offset += 37;
    }

    if (!have_clen) {
        /* Fast integer to ascii without snprintf overhead */
        char num_buf[20];
        char *p = num_buf + sizeof(num_buf);
        size_t tmp_len = body_len;
        do {
            *--p = '0' + (tmp_len % 10);
            tmp_len /= 10;
        } while (tmp_len > 0);
        size_t num_len = (size_t)((num_buf + sizeof(num_buf)) - p);
        if (offset + 16 + num_len + 2 <= buf_size) {
            memcpy(buf + offset, "Content-Length: ", 16);
            memcpy(buf + offset + 16, p, num_len);
            buf[offset + 16 + num_len] = '\r';
            buf[offset + 16 + num_len + 1] = '\n';
            offset += 16 + num_len + 2;
        }
    }

    if (!have_conn) {
        if (res->keep_alive) {
            static const char conn_ka[] = "Connection: keep-alive\r\n";
            if (offset + sizeof(conn_ka) - 1 <= buf_size) {
                memcpy(buf + offset, conn_ka, sizeof(conn_ka) - 1);
                offset += sizeof(conn_ka) - 1;
            }
        } else {
            static const char conn_cl[] = "Connection: close\r\n";
            if (offset + sizeof(conn_cl) - 1 <= buf_size) {
                memcpy(buf + offset, conn_cl, sizeof(conn_cl) - 1);
                offset += sizeof(conn_cl) - 1;
            }
        }
    }

    if (res->alt_svc) {
        if (offset < buf_size) {
            int n = snprintf(buf + offset, buf_size - offset, "Alt-Svc: %s\r\n", res->alt_svc);
            if (n > 0) {
                offset += n;
                if (offset > buf_size) offset = buf_size;
            }
        }
    }

    if (offset + 2 <= buf_size) {
        buf[offset++] = '\r';
        buf[offset++] = '\n';
    }
    return offset;
}

/**
 * @brief Serialize the status line and headers into a caller-provided buffer.
 *
 * Non-static wrapper around serialize_headers() so the TLS send path can
 * stream headers and body separately instead of materializing one blob.
 *
 * @param res Response object to serialize.
 * @param buf Destination buffer for the header block.
 * @param buf_size Total capacity of @p buf in bytes.
 * @return Number of bytes written into the buffer.
 */
size_t cwist_http_serialize_headers(cwist_http_response *res, char *buf, size_t buf_size) {
    return serialize_headers(res, buf, buf_size);
}

#include <sys/uio.h> // For writev and BSD sendfile

/**
 * @brief Send an entire iovec over a (possibly non-blocking) socket.
 * Handles EINTR, EAGAIN/EWOULDBLOCK with POLLOUT polling, and partial writes.
 * @return 0 on success, -1 on fatal error or timeout.
 */
static int cwist_http_sendmsg_all(int fd, struct iovec *iov, int iovcnt, int flags) {
    struct iovec *cur = iov;
    int curcnt = iovcnt;
    while (curcnt > 0) {
        struct msghdr msg = {0};
        msg.msg_iov = cur;
        msg.msg_iovlen = (size_t)curcnt;
        ssize_t n = sendmsg(fd, &msg, flags);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
                if (ret <= 0) return -1;
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
                continue;
            }
            return -1;
        }
        if (n == 0) return -1;

        while (curcnt > 0 && (size_t)n >= cur->iov_len) {
            n -= (ssize_t)cur->iov_len;
            cur++;
            curcnt--;
        }
        if (curcnt > 0) {
            cur->iov_base = (char *)cur->iov_base + n;
            cur->iov_len -= (size_t)n;
        }
    }
    return 0;
}

/**
 * @brief Attempt an optimized file-stream send path using platform sendfile support.
 * @param client_fd Connected client socket descriptor.
 * @param res Response object configured for file streaming.
 * @return true when the file body and headers were transmitted successfully.
 */
static bool cwist_http_stream_file_fast(int client_fd, cwist_http_response *res) {
    if (!res || !res->use_file_stream || res->file_stream_fd < 0) return false;
    size_t remaining = res->file_stream_len;
    off_t offset = res->file_stream_offset;
    while (remaining > 0) {
#if defined(__linux__)
        ssize_t sent = sendfile(client_fd, res->file_stream_fd, &offset, remaining);
        if (sent < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { .fd = client_fd, .events = POLLOUT };
                int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
                if (ret <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
                continue;
            }
            return false;
        }
        if (sent == 0) break;
        remaining -= (size_t)sent;
#else
        /* Portable read+write fallback for macOS, FreeBSD, and other platforms.
           lseek is used to handle the offset since pread avoids modifying the
           file descriptor's position across calls. */
        char buf[65536];
        size_t to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
        ssize_t n = pread(res->file_stream_fd, buf, to_read, offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) break;
        ssize_t nw = 0;
        while (nw < n) {
            ssize_t w = write(client_fd, buf + nw, (size_t)(n - nw));
            if (w < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    struct pollfd pfd = { .fd = client_fd, .events = POLLOUT };
                    int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
                    if (ret <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
                    continue;
                }
                return false;
            }
            nw += w;
        }
        offset += nw;
        remaining -= (size_t)nw;
#endif
    }
    res->file_stream_offset = offset;
    return remaining == 0;
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
    struct iovec iov[2];
    int iov_cnt = 1;

    iov[0].iov_base = header_buf;
    iov[0].iov_len = header_len;

    if (!res->use_file_stream && body_len > 0 && body_ptr) {
        iov[1].iov_base = (void*)body_ptr;
        iov[1].iov_len = body_len;
        iov_cnt = 2;
    }

    int flags = 0;
    #if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
    #endif
    #if defined(MSG_DONTWAIT)
    flags |= MSG_DONTWAIT;
    #endif

    if (cwist_http_sendmsg_all(client_fd, iov, iov_cnt, flags) != 0) {
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
        cwist_sstring_append_len(s, res->body->data, res->body->size);
    }
    return s;
}

/**
 * @brief Parse a raw HTTP request buffer into a CWIST request object.
 * @param raw_request NUL-terminated request buffer containing headers and optional body.
 * @return Parsed request object, or NULL on malformed input.
 */
/**
 * @brief Internal helper to parse request when header_end is already known.
 */
static cwist_http_request *cwist_http_parse_request_with_header_end(const char *raw_request, const char *header_end) {
    if (!raw_request || !header_end) return NULL;

    const char *line_start = raw_request;
    const char *line_end = cwist_simd_find_crlf(line_start, (size_t)(header_end - line_start + 2));
    if (!line_end || line_end > header_end) { 
        return NULL; 
    }

    cwist_http_request *req = cwist_http_request_create();
    if (!req) return NULL;

    // Fast path for root GET / HTTP/1.1\r\n
    if (line_start[0] == 'G' && line_start[1] == 'E' && line_start[2] == 'T' &&
        line_start[3] == ' ' && line_start[4] == '/' && line_start[5] == ' ' &&
        line_start[6] == 'H' && line_start[7] == 'T' && line_start[8] == 'T' &&
        line_start[9] == 'P' && line_start[10] == '/' && line_start[11] == '1' &&
        line_start[12] == '.' && line_start[13] == '1' && line_end == line_start + 14) {
        req->method = CWIST_HTTP_GET;
        cwist_sstring_borrow(req->path, "/", 1);
        cwist_sstring_borrow(req->query, "", 0);
        cwist_sstring_borrow(req->version, "HTTP/1.1", 8);
        req->keep_alive = true;
    } else {
        // 1. Request Line (Optimized: SIMD space search)
        const char *sp1 = cwist_simd_find_char(line_start, (size_t)(line_end - line_start), ' ');
        if (!sp1 || sp1 > line_end) { cwist_http_request_destroy(req); return NULL; }
        const char *sp2 = cwist_simd_find_char(sp1 + 1, (size_t)(line_end - (sp1 + 1)), ' ');
        if (!sp2 || sp2 > line_end) { cwist_http_request_destroy(req); return NULL; }

        req->method = cwist_http_string_to_method_len(line_start, sp1 - line_start);
        
        const char *path_start = sp1 + 1;
        const char *path_end = sp2;
        const char *query_sep = (const char *)memchr(path_start, '?', (size_t)(path_end - path_start));
        
        if (query_sep) {
            cwist_http_sstring_assign_arena(req->path, (cwist_arena_t *)req->arena, path_start, (size_t)(query_sep - path_start));
            cwist_http_sstring_assign_arena(req->query, (cwist_arena_t *)req->arena, query_sep + 1, (size_t)(path_end - (query_sep + 1)));
            req->query_params = cwist_query_map_create_in_arena(req->arena);
            if (req->query_params) {
                cwist_query_map_parse(req->query_params, req->query->data);
            }
        } else {
            cwist_http_sstring_assign_arena(req->path, (cwist_arena_t *)req->arena, path_start, (size_t)(path_end - path_start));
            cwist_http_sstring_assign_arena(req->query, (cwist_arena_t *)req->arena, "", 0);
        }

        cwist_http_sstring_assign_arena(req->version, (cwist_arena_t *)req->arena, sp2 + 1, (size_t)(line_end - (sp2 + 1)));
        if (strncmp(sp2 + 1, "HTTP/1.1", 8) == 0) {
            req->keep_alive = true;
        } else {
            req->keep_alive = false;
        }
    }

    // 2. Headers (SIMD colon and CRLF scanning + SWAR)
    line_start = line_end + 2; 
    while (line_start < header_end) {
        line_end = cwist_simd_find_crlf(line_start, (size_t)(header_end + 2 - line_start));
        if (!line_end || line_end == line_start) break;

        const char *colon = cwist_simd_find_char(line_start, (size_t)(line_end - line_start), ':');
        if (colon) {
            size_t key_len = colon - line_start;
            const char *val_start = colon + 1;
            while (val_start < line_end && *val_start == ' ') val_start++;
            size_t val_len = line_end - val_start;
            
            cwist_http_header_add_ex_len(&req->headers, (cwist_arena_t *)req->arena, line_start, key_len, val_start, val_len);
            
            char k0 = line_start[0];
            if (key_len == 10 && (k0 == 'C' || k0 == 'c')) {
                uint64_t w0;
                memcpy(&w0, line_start, 8);
                if ((w0 | 0x2020202020202020ULL) == 0x697463656e6e6f63ULL) { /* "connecti" */
                    uint16_t w1;
                    memcpy(&w1, line_start + 8, 2);
                    if ((w1 | 0x2020) == 0x6e6f) { /* "on" */
                        if (val_len == 5 && (val_start[0] == 'c' || val_start[0] == 'C')) {
                            if (strncasecmp(val_start, "close", 5) == 0) req->keep_alive = false;
                        } else if (val_len == 10 && (val_start[0] == 'k' || val_start[0] == 'K')) {
                            if (strncasecmp(val_start, "keep-alive", 10) == 0) req->keep_alive = true;
                        }
                    }
                }
            } else if (key_len == 14 && (k0 == 'C' || k0 == 'c')) {
                if (strncasecmp(line_start, "Content-Length", 14) == 0) {
                    size_t len = 0;
                    for (size_t i = 0; i < val_len; i++) {
                        if (val_start[i] >= '0' && val_start[i] <= '9') {
                            len = len * 10 + (val_start[i] - '0');
                        } else {
                            break;
                        }
                    }
                    req->content_length = len;
                }
            }
        }
        line_start = line_end + 2;
    }

    const char *body_start = header_end + 4;
    if (*body_start != '\0') {
        size_t available = strlen(body_start);
        size_t to_take = (req->content_length > 0 && req->content_length < available)
                         ? req->content_length
                         : available;
        cwist_http_sstring_assign_arena(req->body, (cwist_arena_t *)req->arena, body_start, to_take);
    }

    return req;
}

cwist_http_request *cwist_http_parse_request(const char *raw_request) {
    if (!raw_request) return NULL;
    size_t raw_len = strlen(raw_request);
    const char *header_end = cwist_simd_find_crlfcrlf(raw_request, raw_len);
    if (!header_end) return NULL;
    return cwist_http_parse_request_with_header_end(raw_request, header_end);
}



/**
 * @brief Read and reassemble a chunked transfer-encoded body.
 * @return 0 on success, -1 on error.
 */
static int http_parse_chunk_size(const char *line, size_t len, size_t *out_size) {
    size_t value = 0, digits = 0;
    for (size_t i = 0; i < len && line[i] != ';'; ++i) {
        unsigned char c = (unsigned char)line[i];
        unsigned int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return -1;
        if (value > (CWIST_HTTP_MAX_BODY_SIZE - digit) / 16) return -1;
        value = value * 16 + digit;
        ++digits;
    }
    if (!digits) return -1;
    *out_size = value;
    return 0;
}

static int http_read_chunked_body(int client_fd, char *buf, size_t *avail, size_t buf_cap, cwist_sstring *out) {
    size_t offset = 0;

    while (1) {
        char *crlf = memmem(buf + offset, *avail - offset, "\r\n", 2);
        while (!crlf) {
            if (*avail >= buf_cap - 1) return -1;
            ssize_t bytes = recv(client_fd, buf + *avail, buf_cap - 1 - *avail, 0);
            if (bytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
                    int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
                    if (ret <= 0) return -1;
                    continue;
                }
                if (errno == EINTR) continue;
                return -1;
            }
            if (bytes == 0) return -1;
            *avail += (size_t)bytes;
            buf[*avail] = '\0';
            crlf = memmem(buf + offset, *avail - offset, "\r\n", 2);
        }

        size_t line_len = (size_t)(crlf - (buf + offset)) + 2;
        size_t chunk_size = 0;
        /* Strict hexadecimal parsing prevents accepting contaminated framing
         * such as `4junk` or signed/overflowed chunk lengths. */
        if (http_parse_chunk_size(buf + offset, line_len - 2, &chunk_size) != 0) return -1;
        offset += line_len;

        if (chunk_size == 0) {
            /* Consume optional trailers until empty line */
            while (offset + 1 < *avail && !(buf[offset] == '\r' && buf[offset + 1] == '\n')) {
                char *trailer_crlf = memmem(buf + offset, *avail - offset, "\r\n", 2);
                if (!trailer_crlf) {
                    if (*avail >= buf_cap - 1) return -1;
                    ssize_t bytes = recv(client_fd, buf + *avail, buf_cap - 1 - *avail, 0);
                    if (bytes < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
                            int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
                            if (ret <= 0) return -1;
                            continue;
                        }
                        if (errno == EINTR) continue;
                        return -1;
                    }
                    if (bytes == 0) return -1;
                    *avail += (size_t)bytes;
                    buf[*avail] = '\0';
                    trailer_crlf = memmem(buf + offset, *avail - offset, "\r\n", 2);
                }
                if (trailer_crlf) {
                    offset = (size_t)(trailer_crlf - buf) + 2;
                }
            }
            if (offset + 1 < *avail && buf[offset] == '\r' && buf[offset + 1] == '\n') {
                offset += 2;
            }
            break;
        }

        if (chunk_size > CWIST_HTTP_MAX_BODY_SIZE || out->size + chunk_size > CWIST_HTTP_MAX_BODY_SIZE) return -1;

        while (*avail - offset < chunk_size + 2) {
            if (*avail >= buf_cap - 1) return -1;
            ssize_t bytes = recv(client_fd, buf + *avail, buf_cap - 1 - *avail, 0);
            if (bytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
                    int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
                    if (ret <= 0) return -1;
                    continue;
                }
                if (errno == EINTR) continue;
                return -1;
            }
            if (bytes == 0) return -1;
            *avail += (size_t)bytes;
            buf[*avail] = '\0';
        }

        if (buf[offset + chunk_size] != '\r' || buf[offset + chunk_size + 1] != '\n') return -1;
        if (cwist_sstring_append_len(out, buf + offset, chunk_size).error.err_i8 != 0) return -1;
        offset += chunk_size + 2;
    }

    size_t leftover = *avail - offset;
    if (leftover > 0) {
        memmove(buf, buf + offset, leftover);
    }
    *avail = leftover;
    return 0;
}

cwist_http_request *cwist_http_receive_request(int client_fd, char *read_buf, size_t buf_size, size_t *buf_len) {
    size_t total_received = *buf_len;
    char *header_end = NULL;

    // 1. Read until headers are complete
    while (!(header_end = (char *)cwist_simd_find_crlfcrlf(read_buf, total_received))) {
        if (total_received >= buf_size - 1) {
            /* Fat Cookie/Authorization combinations can legitimately push a
             * header block past the read buffer; without this trail the drop
             * is indistinguishable from a client vanish. */
            cwist_metric_inc(cwist_metrics_registry(), CWIST_METRIC_HTTP_HEADER_OVERFLOW);
            CWIST_LOG_WARN("[http] dropping connection: headers exceed %zu-byte read buffer", buf_size);
            return NULL;
        }

        ssize_t bytes = recv(client_fd, read_buf + total_received, buf_size - 1 - total_received, 0);
        if (bytes <= 0) {
            if (bytes < 0 && errno == EINTR) continue;
            return NULL;
        }
        total_received += (size_t)bytes;
        read_buf[total_received] = '\0';
    }

    cwist_http_request *req = cwist_http_parse_request_with_header_end(read_buf, header_end);
    if (!req) return NULL;

    size_t header_len = (size_t)(header_end + 4 - read_buf);
    size_t body_received = total_received - header_len;

    // 2. Read body based on Content-Length or Transfer-Encoding
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

        size_t to_copy = (body_received < (size_t)req->content_length) ? body_received : (size_t)req->content_length;
        memcpy(body, header_end + 4, to_copy);
        size_t current_body_len = to_copy;

        while (current_body_len < (size_t)req->content_length) {
            ssize_t bytes = recv(client_fd, body + current_body_len, (size_t)req->content_length - current_body_len, 0);
            if (bytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
                    int ret = poll(&pfd, 1, CWIST_HTTP_TIMEOUT_MS);
                    if (ret <= 0) {
                        cwist_free(body);
                        cwist_http_request_destroy(req);
                        return NULL;
                    }
                    continue;
                }
                if (errno == EINTR) continue;
                cwist_free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }
            if (bytes == 0) {
                cwist_free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }
            current_body_len += (size_t)bytes;
        }
        body[req->content_length] = '\0';
        /* Adopt the filled buffer: one allocation, zero copies. The partial
         * body the parser staged in the arena is simply superseded. */
        cwist_sstring_adopt_len(req->body, body, (size_t)req->content_length);

        // Calculate leftovers
        if (body_received > (size_t)req->content_length) {
            size_t leftover_len = body_received - (size_t)req->content_length;
            memmove(read_buf, header_end + 4 + req->content_length, leftover_len);
            *buf_len = leftover_len;
        } else {
            *buf_len = 0;
        }
    } else {
        const char *te = cwist_http_header_get(req->headers, "Transfer-Encoding");
        if (te && strcasecmp(te, "chunked") == 0) {
            if (body_received > 0) {
                memmove(read_buf, header_end + 4, body_received);
            }
            *buf_len = body_received;
            read_buf[*buf_len] = '\0';

            cwist_sstring *chunked = cwist_sstring_create();
            if (!chunked) {
                cwist_http_request_destroy(req);
                return NULL;
            }
            if (http_read_chunked_body(client_fd, read_buf, buf_len, buf_size, chunked) != 0) {
                cwist_sstring_destroy(chunked);
                cwist_http_request_destroy(req);
                return NULL;
            }
            /* Move the assembled buffer into the request body instead of
             * copying it a second time. */
            char *chunked_data = chunked->data;
            size_t chunked_len = chunked->size;
            chunked->data = NULL;
            chunked->size = 0;
            cwist_sstring_destroy(chunked);
            cwist_sstring_adopt_len(req->body, chunked_data, chunked_len);
        } else {
            // No body, leftovers are everything after headers
            if (body_received > 0) {
                memmove(read_buf, header_end + 4, body_received);
                *buf_len = body_received;
            } else {
                *buf_len = 0;
            }
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
        buffer = (char *)cwist_alloc(file_size + 1);
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
            /* Adopt the read buffer as the body: no second file-size copy. */
            buffer[file_size] = '\0';
            cwist_sstring_adopt_len(res->body, buffer, file_size);
        } else {
            cwist_sstring_borrow(res->body, "", 0);
        }
    } else {
        cwist_sstring_borrow(res->body, "", 0);
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

  if(!address || inet_pton(AF_INET, address, &sockv4->sin_addr) != 1) {
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
#ifdef SO_NOSIGPIPE
  int no_sig_pipe = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sig_pipe, sizeof(no_sig_pipe));
#endif
#endif

  sockv4->sin_family = AF_INET;
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

    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

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
            int nodelay = 1;
            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
            handle_client_forking(client_fd, handler, ctx);
        }
    }

    if (config->use_threading) {
        if (cwist_http_pool_init() != 0) {
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
                cwist_http_pool_destroy();
                return err;
            }
            int nodelay = 1;
            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
            cwist_http_pool_submit(client_fd, handler, ctx);
        }
        cwist_http_pool_destroy();
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
                            int nodelay = 1;
                            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
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
                            int nodelay = 1;
                            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
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
