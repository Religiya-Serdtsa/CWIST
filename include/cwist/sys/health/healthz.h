/**
 * @file healthz.h
 * @brief Standardised readiness and liveness probing for CWIST.
 *
 * Provides a cwist_app_healthz endpoint that reports the aggregated
 * health of dependent subsystems (database, io_uring backend, memory).
 */

#ifndef __CWIST_HEALTHZ_H__
#define __CWIST_HEALTHZ_H__

#include <cwist/net/http/http.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Probe types
 * ---------------------------------------------------------------------- */

/** @brief Classification of a subsystem health check. */
typedef enum cwist_health_status {
    CWIST_HEALTH_OK,       /**< Subsystem is healthy. */
    CWIST_HEALTH_DEGRADED, /**< Subsystem is functional but impaired. */
    CWIST_HEALTH_FAIL,     /**< Subsystem is down or unreachable. */
} cwist_health_status_t;

/** @brief Result of a single probe. */
typedef struct cwist_health_probe {
    const char *name;           /**< Probe name (e.g., "database"). */
    cwist_health_status_t status;
    const char *message;        /**< Human-readable detail (optional). */
} cwist_health_probe_t;

/** @brief User-provided probe callback. */
typedef cwist_health_probe_t (*cwist_health_probe_fn)(void *ctx);

/* -------------------------------------------------------------------------
 * Registry
 * ---------------------------------------------------------------------- */

/**
 * @brief Register a custom probe function.
 *
 * Probes are evaluated in registration order when /healthz is requested.
 *
 * @param name Probe name exposed in the JSON response.
 * @param fn   Probe callback.
 * @param ctx  Opaque context forwarded to @p fn.
 * @return true on success.
 */
bool cwist_healthz_register(const char *name, cwist_health_probe_fn fn, void *ctx);

/**
 * @brief Unregister a previously registered probe.
 */
void cwist_healthz_unregister(const char *name);

/**
 * @brief Evaluate all probes and return the aggregate status.
 *
 * @param out_probes     [out] Array of probe results (caller provides storage).
 * @param max_probes     Capacity of @p out_probes.
 * @param out_count      [out] Number of probes written.
 * @param out_overall    [out] OK only if every probe is OK; FAIL if any probe is FAIL;
 *                       otherwise DEGRADED.
 */
void cwist_healthz_run(cwist_health_probe_t *out_probes,
                        size_t max_probes,
                        size_t *out_count,
                        cwist_health_status_t *out_overall);

/* -------------------------------------------------------------------------
 * HTTP integration
 * ---------------------------------------------------------------------- */

/**
 * @brief Populate an HTTP response with the healthz JSON payload.
 *
 * Status mapping:
 *  - 200 OK       when overall == CWIST_HEALTH_OK
 *  - 503 Service Unavailable when overall == CWIST_HEALTH_FAIL
 *  - 429 Too Many Requests (re-used as degraded) when overall == CWIST_HEALTH_DEGRADED
 *
 * JSON shape:
 * @code
 * {"status":"ok","probes":[{"name":"database","status":"ok"},...]}
 * @endcode
 *
 * @param res Response to populate.
 */
void cwist_app_healthz(cwist_http_response *res);

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_HEALTHZ_H__ */
