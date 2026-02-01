#ifndef __CWIST_MIDDLEWARE_H__
#define __CWIST_MIDDLEWARE_H__

#include <cwist/sys/app/app.h>

/**
 * @brief Request ID middleware.
 * @param header_name Optional override for the header (defaults to "X-Request-Id").
 */
cwist_middleware_func cwist_mw_request_id(const char *header_name);

/** @brief Access log middleware output formats. */
typedef enum {
    CWIST_LOG_COMMON,
    CWIST_LOG_COMBINED,
    CWIST_LOG_JSON
} cwist_log_format_t;

/** @brief Access log middleware factory. */
cwist_middleware_func cwist_mw_access_log(cwist_log_format_t format);

/** @brief Fixed-window rate limiter middleware (per-IP). */
cwist_middleware_func cwist_mw_rate_limit_ip(int requests_per_minute);

/**
 * @brief CORS middleware.
 * Adds CORS headers and handles OPTIONS requests (returns 204).
 */
cwist_middleware_func cwist_mw_cors(void);

#endif
