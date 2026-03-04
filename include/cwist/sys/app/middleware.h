#ifndef __CWIST_MIDDLEWARE_H__
#define __CWIST_MIDDLEWARE_H__

#include <cwist/sys/app/app.h>
#include <cwist/security/jwt/jwt.h>

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

/**
 * @brief JWT authentication middleware factory.
 *
 * Reads the Authorization header, expects "Bearer <token>".
 * On success the decoded claims are available via the request context.
 * On failure responds with 401 Unauthorized and short-circuits the chain.
 *
 * @param secret HMAC-SHA256 signing secret (null-terminated, must outlive the
 *               middleware invocations — typically a static/global string).
 * @return Middleware function pointer.
 */
cwist_middleware_func cwist_mw_jwt_auth(const char *secret);

/**
 * @brief Retrieve JWT claims stored by cwist_mw_jwt_auth from a request.
 *
 * Only valid inside a handler that sits behind the JWT middleware.
 *
 * @param req The current HTTP request.
 * @return Read-only pointer to the claims, or NULL if not authenticated.
 */
const cwist_jwt_claims *cwist_mw_jwt_get_claims(const cwist_http_request *req);

#endif
