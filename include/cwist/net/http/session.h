/**
 * @file session.h
 * @brief Signed client-side session cookie API.
 */

#ifndef __CWIST_SESSION_H__
#define __CWIST_SESSION_H__

#include <cwist/net/http/http.h>
#include <cwist/sys/app/app.h>
#include <cwist/query.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque session handle attached to a request.
 */
typedef struct cwist_session cwist_session_t;

/**
 * @brief Initialize session handling for an app with a signing secret.
 *
 * If secret is NULL, a random secret is generated. The secret is copied.
 *
 * @param app    Application context.
 * @param secret HMAC-SHA256 secret (at least 32 bytes recommended).
 * @return 0 on success, -1 on failure.
 */
int cwist_app_use_session(cwist_app *app, const char *secret);

/**
 * @brief Set session cookie name. Defaults to "cwist_session".
 */
void cwist_app_set_session_name(cwist_app *app, const char *name);

/**
 * @brief Set session cookie max-age in seconds. Defaults to 86400 (1 day).
 */
void cwist_app_set_session_max_age(cwist_app *app, int seconds);

/**
 * @brief Load or create a session for the current request.
 *
 * Usually called automatically by the session middleware. Handlers can call
 * this if they need session access without the middleware.
 */
cwist_session_t *cwist_session_start(cwist_app *app,
                                     cwist_http_request *req,
                                     cwist_http_response *res);

/**
 * @brief Get a session value by key.
 * @return Pointer to value, or NULL if missing.
 */
const char *cwist_session_get(cwist_session_t *session, const char *key);

/**
 * @brief Set a session value. Value is copied.
 * @return 0 on success, -1 on failure.
 */
int cwist_session_set(cwist_session_t *session, const char *key, const char *value);

/**
 * @brief Delete a session key.
 */
void cwist_session_delete(cwist_session_t *session, const char *key);

/**
 * @brief Persist the session to the response cookie.
 *
 * The session middleware calls this automatically after the handler. If you
 * are not using the middleware, call it before sending the response.
 */
int cwist_session_commit(cwist_session_t *session, cwist_http_response *res);

/**
 * @brief Mark the session for destruction (clears cookie on commit).
 */
void cwist_session_invalidate(cwist_session_t *session);

/**
 * @brief Destroy a session handle and its data.
 */
void cwist_session_destroy(cwist_session_t *session);

/**
 * @brief Session middleware factory.
 */
cwist_middleware_func cwist_mw_session(cwist_app *app);

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_SESSION_H__ */
