/**
 * @file metrics.h
 * @brief Production-grade metrics engine for CWIST.
 *
 * Exposes Prometheus-compatible text format via a /metrics endpoint.
 * All counters are updated with lock-free atomic operations to ensure
 * zero performance degradation on the hot path.
 */

#ifndef __CWIST_METRICS_H__
#define __CWIST_METRICS_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

struct cwist_http_response;

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Metric types
 * ---------------------------------------------------------------------- */

/** @brief Supported Prometheus metric kinds. */
typedef enum cwist_metric_type {
    CWIST_METRIC_COUNTER,   /**< Monotonically increasing counter. */
    CWIST_METRIC_GAUGE,     /**< Arbitrary value that can go up or down. */
    CWIST_METRIC_HISTOGRAM, /**< Bucketed observations (simplified). */
} cwist_metric_type_t;

/** @brief Atomic metric value container. */
typedef struct cwist_metric_value {
    atomic_uintmax_t raw;   /**< Base-1000 scaled integer for gauge/counter. */
} cwist_metric_value_t;

/** @brief Single metric definition. */
typedef struct cwist_metric {
    const char *name;       /**< Prometheus metric name (snake_case). */
    const char *help;       /**< HELP text line. */
    cwist_metric_type_t type;
    cwist_metric_value_t value;
    atomic_uintmax_t count; /**< Sample count (for histograms / rate calc). */
    atomic_uintmax_t sum;   /**< Sum of observations (for histograms). */
} cwist_metric_t;

/** @brief Pre-defined framework metrics. */
typedef enum cwist_metric_id {
    CWIST_METRIC_URING_SQES_PENDING,      /**< Active SQEs waiting for kernel. */
    CWIST_METRIC_URING_BUFFERS_ACTIVE,    /**< In-flight fixed buffers. */
    CWIST_METRIC_STREAMS_HTTP2,           /**< Active HTTP/2 streams. */
    CWIST_METRIC_STREAMS_HTTP3,           /**< Active HTTP/3 streams. */
    CWIST_METRIC_FLOW_WINDOW_HEALTH,      /**< Flow-control window health (0-100). */
    CWIST_METRIC_VALIDATION_ERRORS,       /**< Validation error frequency. */
    CWIST_METRIC_REQUESTS_TOTAL,          /**< Total HTTP requests handled. */
    CWIST_METRIC_REQUEST_DURATION_NS,     /**< Request duration (nanoseconds sum). */
    CWIST_METRIC_HTTP_HEADER_OVERFLOW,    /**< H1 connections dropped: headers exceeded read buffer. */
    CWIST_METRIC_H2_HEADERS_DROPPED,      /**< H2 header fields dropped: unresolvable HPACK index. */
    CWIST_METRIC_COUNT
} cwist_metric_id_t;

/* -------------------------------------------------------------------------
 * Registry API
 * ---------------------------------------------------------------------- */

/**
 * @brief Global metrics registry (singleton, lazy-initialised).
 *
 * The registry is safe to access from multiple threads without external
 * locking because every update is an atomic operation.
 */

typedef struct cwist_metrics_registry cwist_metrics_registry_t;

/**
 * @brief Obtain the global registry, creating it on first call.
 * @return Pointer to the global registry.
 */
cwist_metrics_registry_t *cwist_metrics_registry(void);

/**
 * @brief Reset all metrics in the registry to zero.
 * @param reg Registry to clear.
 */
void cwist_metrics_reset(cwist_metrics_registry_t *reg);

/* -------------------------------------------------------------------------
 * Lock-free updates
 * ---------------------------------------------------------------------- */

/**
 * @brief Increment a counter metric by 1.
 * @param reg Registry.
 * @param id  Metric identifier.
 */
void cwist_metric_inc(cwist_metrics_registry_t *reg, cwist_metric_id_t id);

/**
 * @brief Increment a counter metric by @p delta.
 * @param reg   Registry.
 * @param id    Metric identifier.
 * @param delta Amount to add.
 */
void cwist_metric_add(cwist_metrics_registry_t *reg, cwist_metric_id_t id, uintmax_t delta);

/**
 * @brief Set a gauge metric to an absolute value.
 * @param reg   Registry.
 * @param id    Metric identifier.
 * @param value New absolute value (internally scaled by 1000).
 */
void cwist_metric_set(cwist_metrics_registry_t *reg, cwist_metric_id_t id, long double value);

/**
 * @brief Observe a value into a histogram-like metric.
 * @param reg   Registry.
 * @param id    Metric identifier.
 * @param value Observed value.
 */
void cwist_metric_observe(cwist_metrics_registry_t *reg, cwist_metric_id_t id, long double value);

/**
 * @brief Atomically load the current raw value of a metric.
 * @param reg Registry.
 * @param id  Metric identifier.
 * @return Current raw value.
 */
uintmax_t cwist_metric_load(const cwist_metrics_registry_t *reg, cwist_metric_id_t id);

/* -------------------------------------------------------------------------
 * Prometheus exposition
 * ---------------------------------------------------------------------- */

/**
 * @brief Render all metrics in Prometheus text exposition format.
 *
 * The returned string is heap-allocated and must be freed by the caller
 * using standard free().
 *
 * @param reg Registry to render.
 * @return Null-terminated Prometheus text, or NULL on OOM.
 */
char *cwist_metrics_render_prometheus(const cwist_metrics_registry_t *reg);

/**
 * @brief Convenience helper: populate an HTTP response with /metrics body.
 *
 * Sets Content-Type to text/plain; version=0.0.4 and fills the body
 * with the current Prometheus snapshot.
 *
 * @param res Response to populate.
 */
void cwist_metrics_serve_http(struct cwist_http_response *res);

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_METRICS_H__ */
