/**
 * @file http_client.h
 * @brief Unified HTTP/1.1 and HTTP/2 client using libcurl.
 *
 * Provides a CWIST-native wrapper around libcurl for synchronous
 * HTTP requests.  Supports connection reuse, Alt-Svc upgrade to HTTP/3,
 * redirects, cookies, and TLS via the system libcurl (linked against
 * BoringSSL + nghttp2).
 */

#ifndef __CWIST_HTTP_CLIENT_H__
#define __CWIST_HTTP_CLIENT_H__

#include <cwist/net/http/http.h>
#include <cwist/sys/err/cwist_err.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque HTTP client handle.
 *
 * Internally holds a libcurl multi handle for connection reuse
 * and a share handle for DNS/cookie/SSL session caching.
 */
typedef struct cwist_http_client cwist_http_client;

/** @name Lifecycle */
/** @{ */

/**
 * @brief Create a new HTTP client.
 *
 * Initializes libcurl global state on first call (ref-counted).
 */
cwist_http_client *cwist_http_client_create(void);

/**
 * @brief Destroy an HTTP client and release all associated resources.
 */
void cwist_http_client_destroy(cwist_http_client *client);

/** @} */

/** @name Configuration */
/** @{ */

/**
 * @brief Enable or disable automatic redirect following.
 * @param client Client handle.
 * @param follow Non-zero to follow redirects (default: 1).
 */
void cwist_http_client_set_follow_redirects(cwist_http_client *client, int follow);

/**
 * @brief Set request timeout in milliseconds.
 * @param client Client handle.
 * @param timeout_ms Timeout value, 0 for no timeout (default: 30000).
 */
void cwist_http_client_set_timeout_ms(cwist_http_client *client, int timeout_ms);

/**
 * @brief Set path to CA bundle for TLS verification.
 * @param client Client handle.
 * @param path Filesystem path to PEM CA bundle, or NULL for system default.
 */
void cwist_http_client_set_ca_bundle(cwist_http_client *client, const char *path);

/**
 * @brief Enable or disable Alt-Svc caching for HTTP/3 upgrade.
 *
 * When enabled, libcurl caches Alt-Svc responses and may transparently
 * upgrade HTTP/2 connections to HTTP/3 on subsequent requests.
 *
 * @param client Client handle.
 * @param enabled Non-zero to enable (default: 0).
 */
void cwist_http_client_enable_altsvc(cwist_http_client *client, int enabled);

/**
 * @brief Set Alt-Svc cache database file path.
 *
 * If set, Alt-Svc entries are persisted across process restarts.
 *
 * @param client Client handle.
 * @param path Filesystem path, or NULL for in-memory only.
 */
void cwist_http_client_set_altsvc_db(cwist_http_client *client, const char *path);

/** @} */

/** @name Request Execution */
/** @{ */

/**
 * @brief Perform a synchronous HTTP request.
 *
 * The request is executed using libcurl's easy interface.  HTTP/2
 * multiplexing is used automatically when the server supports it.
 * If Alt-Svc is enabled and the server previously advertised h3,
 * libcurl may upgrade to HTTP/3 transparently.
 *
 * @param client    Client handle.
 * @param url       Fully-qualified URL (e.g. "https://example.com/path").
 * @param method    HTTP method enum.
 * @param headers   Optional request headers linked list (may be NULL).
 * @param body      Optional request body (may be NULL).
 * @param body_len  Length of @p body in bytes.
 * @param out_response  Output pointer for the parsed response.  Caller
 *                      must destroy with cwist_http_response_destroy().
 * @return CWIST error, err_i16 == 0 on success.
 */
cwist_error_t cwist_http_client_request(cwist_http_client *client,
                                        const char *url,
                                        cwist_http_method_t method,
                                        cwist_http_header_node *headers,
                                        const char *body,
                                        size_t body_len,
                                        cwist_http_response **out_response);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
