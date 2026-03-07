#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>

#include <cwist/sys/app/app.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/mem/gc.h>
#include <cwist/core/mem/alloc.h>
#include <ttak/mem/mem.h>
#include <ttak/timing/timing.h>
#include <ttak/thread/thread_compat.h>
#include <pthread.h>

#define SERVER_PORT 8080

static atomic_uint_fast64_t g_request_count = 0;

typedef struct {
    char *ptr;
    size_t len;
} rps_payload_t;

static cwist_gc_t g_payload_gc;
static pthread_mutex_t g_payload_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(rps_payload_t *) g_payload = NULL;

static void *stats_loop(void *arg) {
    (void)arg;
    uint64_t prev = 0;
    while (1) {
        sleep(1);
        uint64_t now = atomic_load_explicit(&g_request_count, memory_order_relaxed);
        printf("[stats] ~%lu req/s (total=%lu)\n", now - prev, now);
        prev = now;
    }
    return NULL;
}

static void rps_payload_release(rps_payload_t *payload) {
    if (!payload) return;
    if (payload->ptr) {
        cwist_reg_ptr_sized(&g_payload_gc, payload->ptr, payload->len);
    }
    cwist_free(payload);
}

static void rps_payload_refresh(void) {
    pthread_mutex_lock(&g_payload_lock);
    if (!g_payload_gc.initialized) {
        cwist_gc(&g_payload_gc, true);
    }

    const size_t cap = 96;
    uint64_t now = ttak_get_tick_count();
    char *buffer = (char *)ttak_mem_alloc_safe(
        cap, __TTAK_UNSAFE_MEM_FOREVER__, now,
        true, false, true, true, TTAK_MEM_CACHE_ALIGNED
    );
    if (!buffer) {
        pthread_mutex_unlock(&g_payload_lock);
        return;
    }

    int written = snprintf(buffer, cap, "{\"status\":\"ok\",\"tick\":%lu}", now);
    if (written < 0) {
        cwist_reg_ptr_sized(&g_payload_gc, buffer, cap);
        pthread_mutex_unlock(&g_payload_lock);
        return;
    }

    rps_payload_t *payload = cwist_alloc(sizeof(*payload));
    if (!payload) {
        cwist_reg_ptr_sized(&g_payload_gc, buffer, cap);
        pthread_mutex_unlock(&g_payload_lock);
        return;
    }
    payload->ptr = buffer;
    payload->len = (size_t)written;

    rps_payload_t *old = atomic_exchange_explicit(&g_payload, payload, memory_order_acq_rel);
    if (old) {
        rps_payload_release(old);
    }
    cwist_gc_rotate(&g_payload_gc);
    pthread_mutex_unlock(&g_payload_lock);
}

static void rps_payload_destroy(void) {
    pthread_mutex_lock(&g_payload_lock);
    rps_payload_t *payload = atomic_exchange_explicit(&g_payload, NULL, memory_order_acq_rel);
    if (payload) {
        rps_payload_release(payload);
        cwist_gc_rotate(&g_payload_gc);
    }
    pthread_mutex_unlock(&g_payload_lock);
    cwist_gc_shutdown(&g_payload_gc);
}

static void rps_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    rps_payload_t *payload = atomic_load_explicit(&g_payload, memory_order_acquire);
    if (payload && payload->ptr) {
        cwist_http_response_set_body_ptr(res, payload->ptr, payload->len);
        cwist_http_header_add(&res->headers, "Content-Type", "application/json");
        res->status_code = CWIST_HTTP_OK;
        atomic_fetch_add_explicit(&g_request_count, 1, memory_order_relaxed);
        return;
    }
    cwist_sstring_assign(res->body, "payload not ready");
    res->status_code = CWIST_HTTP_SERVICE_UNAVAILABLE;
}

static void refresh_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    rps_payload_refresh();
    cwist_sstring_assign(res->body, "{\"message\":\"payload refreshed\"}");
    res->status_code = CWIST_HTTP_OK;
}

static void index_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "CWIST Managed Server\n/rps\n/refresh\n");
    res->status_code = CWIST_HTTP_OK;
}

int main(void) {
    rps_payload_refresh();

    ttak_thread_t st;
    ttak_thread_create(&st, stats_loop, NULL);
    ttak_thread_detach(st);

    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/", index_handler);
    cwist_app_get(app, "/rps", rps_handler);
    cwist_app_get(app, "/refresh", refresh_handler);

    printf("[System] Service listening on http://0.0.0.0:%d\n", SERVER_PORT);
    int code = cwist_app_listen(app, SERVER_PORT);

    cwist_app_destroy(app);
    rps_payload_destroy();
    return code;
}
