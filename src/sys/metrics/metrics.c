/**
 * @file metrics.c
 * @brief Lock-free metrics registry and Prometheus exposition.
 */

#include <cwist/sys/metrics/metrics.h>
#include <cwist/net/http/http.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Internal registry layout
 * ---------------------------------------------------------------------- */

struct cwist_metrics_registry {
    cwist_metric_t metrics[CWIST_METRIC_COUNT];
};

static const cwist_metric_t metric_defaults[CWIST_METRIC_COUNT] = {
    [CWIST_METRIC_URING_SQES_PENDING]   = { .name = "cwist_io_uring_sqes_pending",   .help = "Active io_uring SQEs waiting for kernel pickup",   .type = CWIST_METRIC_COUNTER },
    [CWIST_METRIC_URING_BUFFERS_ACTIVE] = { .name = "cwist_io_uring_buffers_active", .help = "In-flight fixed buffers in io_uring pool",         .type = CWIST_METRIC_GAUGE   },
    [CWIST_METRIC_STREAMS_HTTP2]        = { .name = "cwist_streams_http2",           .help = "Active HTTP/2 streams",                            .type = CWIST_METRIC_GAUGE   },
    [CWIST_METRIC_STREAMS_HTTP3]        = { .name = "cwist_streams_http3",           .help = "Active HTTP/3 streams",                            .type = CWIST_METRIC_GAUGE   },
    [CWIST_METRIC_FLOW_WINDOW_HEALTH]   = { .name = "cwist_flow_window_health",      .help = "Flow-control window health (0-100)",               .type = CWIST_METRIC_GAUGE   },
    [CWIST_METRIC_VALIDATION_ERRORS]    = { .name = "cwist_validation_errors",       .help = "Validation error frequency",                       .type = CWIST_METRIC_COUNTER },
    [CWIST_METRIC_REQUESTS_TOTAL]       = { .name = "cwist_requests_total",          .help = "Total HTTP requests handled",                      .type = CWIST_METRIC_COUNTER },
    [CWIST_METRIC_REQUEST_DURATION_NS]  = { .name = "cwist_request_duration_ns",     .help = "Request duration sum in nanoseconds",              .type = CWIST_METRIC_COUNTER },
    [CWIST_METRIC_HTTP_HEADER_OVERFLOW] = { .name = "cwist_http_header_overflow_total", .help = "HTTP/1.1 connections dropped because headers exceeded the read buffer", .type = CWIST_METRIC_COUNTER },
    [CWIST_METRIC_H2_HEADERS_DROPPED]   = { .name = "cwist_h2_headers_dropped_total",   .help = "HTTP/2 header fields dropped due to unresolvable HPACK index",          .type = CWIST_METRIC_COUNTER },
};

/* -------------------------------------------------------------------------
 * Singleton registry
 * ---------------------------------------------------------------------- */

cwist_metrics_registry_t *cwist_metrics_registry(void) {
    static cwist_metrics_registry_t *g_reg = NULL;
    if (__builtin_expect(g_reg != NULL, 1)) return g_reg;

    cwist_metrics_registry_t *alloc = cwist_alloc(sizeof(*alloc));
    if (!alloc) return NULL;
    memcpy(alloc->metrics, metric_defaults, sizeof(metric_defaults));
    for (int i = 0; i < CWIST_METRIC_COUNT; ++i) {
        atomic_init(&alloc->metrics[i].value.raw, 0);
        atomic_init(&alloc->metrics[i].count, 0);
        atomic_init(&alloc->metrics[i].sum, 0);
    }
    g_reg = alloc;
    return g_reg;
}

void cwist_metrics_reset(cwist_metrics_registry_t *reg) {
    if (!reg) return;
    for (int i = 0; i < CWIST_METRIC_COUNT; ++i) {
        atomic_store_explicit(&reg->metrics[i].value.raw, 0, memory_order_relaxed);
        atomic_store_explicit(&reg->metrics[i].count, 0, memory_order_relaxed);
        atomic_store_explicit(&reg->metrics[i].sum, 0, memory_order_relaxed);
    }
}

/* -------------------------------------------------------------------------
 * Lock-free updates
 * ---------------------------------------------------------------------- */

void cwist_metric_inc(cwist_metrics_registry_t *reg, cwist_metric_id_t id) {
    if (!reg || id < 0 || id >= CWIST_METRIC_COUNT) return;
    atomic_fetch_add_explicit(&reg->metrics[id].value.raw, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&reg->metrics[id].count, 1, memory_order_relaxed);
}

void cwist_metric_add(cwist_metrics_registry_t *reg, cwist_metric_id_t id, uintmax_t delta) {
    if (!reg || id < 0 || id >= CWIST_METRIC_COUNT) return;
    atomic_fetch_add_explicit(&reg->metrics[id].value.raw, delta, memory_order_relaxed);
    atomic_fetch_add_explicit(&reg->metrics[id].count, 1, memory_order_relaxed);
}

void cwist_metric_set(cwist_metrics_registry_t *reg, cwist_metric_id_t id, long double value) {
    if (!reg || id < 0 || id >= CWIST_METRIC_COUNT) return;
    uintmax_t scaled = (uintmax_t)roundl(value * 1000.0L);
    atomic_store_explicit(&reg->metrics[id].value.raw, scaled, memory_order_relaxed);
}

void cwist_metric_observe(cwist_metrics_registry_t *reg, cwist_metric_id_t id, long double value) {
    if (!reg || id < 0 || id >= CWIST_METRIC_COUNT) return;
    uintmax_t scaled = (uintmax_t)roundl(value * 1000.0L);
    atomic_fetch_add_explicit(&reg->metrics[id].value.raw, scaled, memory_order_relaxed);
    atomic_fetch_add_explicit(&reg->metrics[id].count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&reg->metrics[id].sum, scaled, memory_order_relaxed);
}

uintmax_t cwist_metric_load(const cwist_metrics_registry_t *reg, cwist_metric_id_t id) {
    if (!reg || id < 0 || id >= CWIST_METRIC_COUNT) return 0;
    return atomic_load_explicit(&reg->metrics[id].value.raw, memory_order_acquire);
}

/* -------------------------------------------------------------------------
 * Prometheus exposition
 * ---------------------------------------------------------------------- */

static const char *type_str(cwist_metric_type_t t) {
    switch (t) {
        case CWIST_METRIC_COUNTER:   return "counter";
        case CWIST_METRIC_GAUGE:     return "gauge";
        case CWIST_METRIC_HISTOGRAM: return "histogram";
        default:                     return "unknown";
    }
}

char *cwist_metrics_render_prometheus(const cwist_metrics_registry_t *reg) {
    if (!reg) return NULL;

    size_t cap = 4096;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t len = 0;

    for (int i = 0; i < CWIST_METRIC_COUNT; ++i) {
        const cwist_metric_t *m = &reg->metrics[i];
        uintmax_t raw = atomic_load_explicit(&m->value.raw, memory_order_acquire);
        uintmax_t cnt = atomic_load_explicit(&m->count, memory_order_acquire);

        int need = snprintf(NULL, 0,
                            "# HELP %s %s\n"
                            "# TYPE %s %s\n"
                            "%s %lu.%03lu\n"
                            "%s_count %lu\n\n",
                            m->name, m->help,
                            m->name, type_str(m->type),
                            m->name,
                            (unsigned long)(raw / 1000),
                            (unsigned long)(raw % 1000),
                            m->name,
                            (unsigned long)cnt);
        if (need < 0) { free(buf); return NULL; }

        size_t required = (size_t)need + 1;
        if (required > cap - len) {
            size_t new_cap = cap;
            while (required > new_cap - len) new_cap *= 2;
            char *nb = realloc(buf, new_cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
            cap = new_cap;
        }

        int written = snprintf(buf + len, cap - len,
                               "# HELP %s %s\n"
                               "# TYPE %s %s\n"
                               "%s %lu.%03lu\n"
                               "%s_count %lu\n\n",
                               m->name, m->help,
                               m->name, type_str(m->type),
                               m->name,
                               (unsigned long)(raw / 1000),
                               (unsigned long)(raw % 1000),
                               m->name,
                               (unsigned long)cnt);
        if (written < 0 || (size_t)written >= cap - len) { free(buf); return NULL; }
        len += (size_t)written;
    }

    buf[len] = '\0';
    return buf;
}

void cwist_metrics_serve_http(cwist_http_response *res) {
    if (!res) return;
    cwist_metrics_registry_t *reg = cwist_metrics_registry();
    char *text = cwist_metrics_render_prometheus(reg);
    if (!text) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "metrics render failed");
        return;
    }
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, text);
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain; version=0.0.4; charset=utf-8");
    free(text);
}
