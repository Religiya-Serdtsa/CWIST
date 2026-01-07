#ifndef __CWIST_MIDDLEWARE_H__
#define __CWIST_MIDDLEWARE_H__

#include <cwist/sys/app/app.h>

// Request ID Middleware
// If header_name is NULL, defaults to "X-Request-Id"
cwist_middleware_func cwist_mw_request_id(const char *header_name);

// Access Log Middleware
typedef enum {
    CWIST_LOG_COMMON,
    CWIST_LOG_COMBINED,
    CWIST_LOG_JSON
} cwist_log_format_t;

cwist_middleware_func cwist_mw_access_log(cwist_log_format_t format);

// Rate Limiter Middleware
// Simple fixed-window rate limiter by IP for now
cwist_middleware_func cwist_mw_rate_limit_ip(int requests_per_minute);

// CORS Middleware
// Adds Cross-Origin Resource Sharing headers.
// Handles OPTIONS requests by returning 204 and allowed headers (short-circuit).
cwist_middleware_func cwist_mw_cors(void);

#endif
