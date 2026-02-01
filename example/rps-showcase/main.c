#include <cwist/sys/app/app.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/macros.h>

#include <ttak/mem/detachable.h>
#include <ttak/mem/epoch.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <unistd.h>

#define RPS_PORT 8080
#define SNAPSHOT_CAPACITY 1024

typedef struct rps_payload_snapshot {
    ttak_detachable_allocation_t alloc;
    size_t payload_len;
    uint64_t version;
} rps_payload_snapshot_t;

typedef struct epoch_pin {
    bool active;
} epoch_pin_t;

static ttak_detachable_context_t g_detachable_ctx;
static bool g_ctx_initialized = false;
static _Atomic(uintptr_t) g_payload_snapshot = 0;
static atomic_uint_fast64_t g_request_count = 0;
static atomic_uint_fast64_t g_refresh_count = 0;
static pthread_t g_stats_thread;

static void rps_payload_cleanup(void *ptr) {
    rps_payload_snapshot_t *snap = (rps_payload_snapshot_t *)ptr;
    if (!snap) return;
    if (g_ctx_initialized) {
        ttak_detachable_mem_free(&g_detachable_ctx, &snap->alloc);
    }
    free(snap);
}

/**
 * Rebuilds the JSON payload and updates the global snapshot atomically.
 * Uses ttak_epoch to retire the old snapshot safely.
 */
static void rps_payload_refresh(const char *reason) {
    if (!g_ctx_initialized) return;

    rps_payload_snapshot_t *snap = calloc(1, sizeof(*snap));
    if (!snap) {
        fprintf(stderr, "[rps] failed to allocate snapshot\n");
        return;
    }

    /* Allocate from detachable memory with current version as tag */
    snap->alloc = ttak_detachable_mem_alloc(&g_detachable_ctx, SNAPSHOT_CAPACITY,
                        atomic_load_explicit(&g_refresh_count, memory_order_relaxed));
    if (!snap->alloc.data) {
        fprintf(stderr, "[rps] detachable allocation failed\n");
        free(snap);
        return;
    }

    uint64_t refresh_id = atomic_fetch_add_explicit(&g_refresh_count, 1, memory_order_relaxed) + 1;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double refreshed_at = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;

    int written = snprintf((char *)snap->alloc.data, snap->alloc.size,
                   "{"
                   "\"message\":\"cwist rps showcase\","
                   "\"version\":%" PRIu64 ","
                   "\"refresh_reason\":\"%s\","
                   "\"refreshed_at\":%.3f,"
                   "\"requests_served\":%" PRIu64
                   "}",
            refresh_id,
            reason ? reason : "manual",
            refreshed_at,
            atomic_load_explicit(&g_request_count, memory_order_relaxed));

    if (written < 0) {
        snap->payload_len = 0;
    } else if ((size_t)written >= snap->alloc.size) {
        snap->payload_len = snap->alloc.size - 1;
    } else {
        snap->payload_len = (size_t)written;
    }
    snap->version = refresh_id;

    /* Registering current thread is required for epoch operations */
    ttak_epoch_register_thread();

    /* Swap the new snapshot with the old one using release semantics */
    uintptr_t prev = atomic_exchange_explicit(&g_payload_snapshot, (uintptr_t)snap, memory_order_acq_rel);
    if (prev) {
        /* Move the old pointer to the retirement list */
        ttak_epoch_retire((void *)prev, rps_payload_cleanup);

        /**
         * CRITICAL: Do not call ttak_epoch_reclaim() here during high load.
         * Let the GC thread or subsequent quiescent states handle reclamation
         * to avoid premature free during asynchronous socket I/O in cwist.
         */
    }
    ttak_epoch_deregister_thread();
}

static void *stats_loop(void *arg) {
    CWIST_UNUSED(arg);
    uint64_t prev = 0;
    while (true) {
        sleep(1);
        uint64_t now = atomic_load_explicit(&g_request_count, memory_order_relaxed);
        printf("[stats] ~%" PRIu64 " req/s (total=%" PRIu64 ")\n", now - prev, now);
        prev = now;

        /**
         * Periodic reclamation in a dedicated thread is safer under heavy load.
         * This ensures thatRetired memory is eventually freed without
         * interrupting the hot path of request handling.
         */
        if (g_ctx_initialized) {
            ttak_epoch_register_thread();
            ttak_epoch_reclaim();
            ttak_epoch_deregister_thread();
        }
    }
    return NULL;
}

static void rps_bootstrap(void) {
    if (g_ctx_initialized) return;
    uint32_t flags = TTAK_ARENA_HAS_EPOCH_RECLAMATION |
    TTAK_ARENA_HAS_DEFAULT_EPOCH_GC |
    TTAK_ARENA_USE_LOCKED_ACCESS;
    ttak_detachable_context_init(&g_detachable_ctx, flags);
    g_ctx_initialized = true;
    rps_payload_refresh("startup");
    if (pthread_create(&g_stats_thread, NULL, stats_loop, NULL) == 0) {
        pthread_detach(g_stats_thread);
    }
}

/**
 * Callback invoked by cwist after the HTTP response body is fully sent or
 * the connection is closed. This is the only safe place to exit the epoch.
 */
static void release_epoch_pin(const void *ptr, size_t len, void *ctx) {
    CWIST_UNUSED(ptr);
    CWIST_UNUSED(len);
    epoch_pin_t *pin = (epoch_pin_t *)ctx;
    if (pin && pin->active) {
        /* Release the protection on the memory segment */
        ttak_epoch_exit();

        /**
         * NOTE: We avoid ttak_epoch_deregister_thread() here because cwist
         * worker threads are pooled. Re-registering/deregistering on every
         * request adds unnecessary overhead and potential TLS corruption.
         */
        pin->active = false;
    }
    free(pin);
}

static void rps_handler(cwist_http_request *req, cwist_http_response *res) {
    CWIST_UNUSED(req);

    /**
     * Ensure the worker thread is recognized by the epoch system.
     * Registering an already registered thread should be idempotent in libttak.
     */
    ttak_epoch_register_thread();
    ttak_epoch_enter();

    /* Load the latest snapshot pointer with acquire semantics */
    uintptr_t raw = atomic_load_explicit(&g_payload_snapshot, memory_order_acquire);
    rps_payload_snapshot_t *snap = (rps_payload_snapshot_t *)raw;

    if (!snap) {
        ttak_epoch_exit();
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "payload unavailable");
        return;
    }

    epoch_pin_t *pin = calloc(1, sizeof(*pin));
    if (!pin) {
        ttak_epoch_exit();
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "failed to pin epoch");
        return;
    }

    pin->active = true;

    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store");

    /**
     * ZERO-COPY: Pass the raw pointer to cwist.
     * The release_epoch_pin callback ensures the thread 'exits' the epoch
     * only after the data is physically transmitted through the socket.
     */
    cwist_http_response_set_body_ptr_managed(res, snap->alloc.data, snap->payload_len, release_epoch_pin, pin);

    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->status_text, "OK");
    res->keep_alive = true;

    atomic_fetch_add_explicit(&g_request_count, 1, memory_order_relaxed);
}

static void refresh_handler(cwist_http_request *req, cwist_http_response *res) {
    const char *reason = (req && req->query && req->query->data && req->query->size > 0)
    ? req->query->data : "manual";
    rps_payload_refresh(reason);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, "{\"ok\":true,\"message\":\"payload refreshed\"}");
}

static void index_handler(cwist_http_request *req, cwist_http_response *res) {
    CWIST_UNUSED(req);
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain; charset=utf-8");
    char info[512];
    snprintf(info, sizeof(info),
         "CWIST RPS showcase (Production Grade)\n"
         "GET /rps    -> Zero-copy JSON via libttak detachable memory\n"
         "GET /refresh -> Retire old snapshot safely\n");
    cwist_sstring_assign(res->body, info);
}

int main(void) {
    rps_bootstrap();

    cwist_app *app = cwist_app_create();
    if (!app) return 1;

    cwist_app_set_max_memspace(app, CWIST_MIB(32));
    cwist_app_get(app, "/", index_handler);
    cwist_app_get(app, "/rps", rps_handler);
    cwist_app_get(app, "/refresh", refresh_handler);

    printf("[rps] listening on http://0.0.0.0:%d\n", RPS_PORT);
    int code = cwist_app_listen(app, RPS_PORT);

    cwist_app_destroy(app);
    return code;
}
