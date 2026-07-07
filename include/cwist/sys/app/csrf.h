/**
 * @file csrf.h
 * @brief CSRF protection via double-submit cookie pattern.
 */

#ifndef __CWIST_CSRF_H__
#define __CWIST_CSRF_H__

#include <cwist/sys/app/app.h>
#include <cwist/net/http/http.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief CSRF middleware factory.
 *
 * On safe requests (GET/HEAD/OPTIONS) a `csrf_token` cookie is issued if
 * missing. On unsafe requests the `X-CSRF-Token` header, `_csrf` form field,
 * or `csrf_token` query parameter must match the cookie value.
 *
 * @param app Application context.
 * @return Middleware function pointer.
 */
cwist_middleware_func cwist_mw_csrf(cwist_app *app);

/**
 * @brief Get the CSRF token for the current request.
 *
 * Useful for rendering forms or meta tags. Returns NULL if the middleware
 * has not yet populated the token.
 *
 * @param req Current request.
 * @return Token string, or NULL.
 */
const char *cwist_csrf_token(cwist_http_request *req);

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_CSRF_H__ */
