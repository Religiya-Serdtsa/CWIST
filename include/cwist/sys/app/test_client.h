/**
 * @file test_client.h
 * @brief In-process HTTP test client (ala Flask test_client).
 */

#ifndef __CWIST_TEST_CLIENT_H__
#define __CWIST_TEST_CLIENT_H__

#include <cwist/sys/app/app.h>
#include <stddef.h>

typedef struct cwist_test_client_cookie {
    char *name;
    char *value;
    char *path;
    struct cwist_test_client_cookie *next;
} cwist_test_client_cookie;

typedef struct cwist_test_client {
    cwist_app *app;
    cwist_test_client_cookie *cookies;
} cwist_test_client;

/** @brief A single key/value pair for headers or ad-hoc cookies. */
typedef struct cwist_test_client_kv {
    const char *key;
    const char *value;
} cwist_test_client_kv;

/** @brief Optional parameters for advanced test requests. */
typedef struct cwist_test_client_request_options {
    const char *body;               /**< Request body (may be NULL). */
    const char *content_type;       /**< Content-Type header value (may be NULL). */
    const cwist_test_client_kv *headers; /**< Extra request headers. */
    size_t header_count;            /**< Number of extra headers. */
    const cwist_test_client_kv *cookies; /**< Ad-hoc request cookies (not jarred). */
    size_t cookie_count;            /**< Number of ad-hoc cookies. */
    const char *query_string;       /**< Raw query string, e.g. "foo=bar" (may be NULL). */
} cwist_test_client_request_options;

/** @name Lifecycle */
/** @{ */

/**
 * @brief Create a test client for an app.
 */
cwist_test_client *cwist_test_client_create(cwist_app *app);

/**
 * @brief Destroy a test client.
 */
void cwist_test_client_destroy(cwist_test_client *client);

/** @} */

/** @name HTTP Methods */
/** @{ */

/**
 * @brief Perform a GET request.
 */
cwist_http_response *cwist_test_client_get(cwist_test_client *client, const char *path);

/**
 * @brief Perform a POST request.
 */
cwist_http_response *cwist_test_client_post(cwist_test_client *client, const char *path, const char *body);

/**
 * @brief Perform a POST request with JSON content-type.
 */
cwist_http_response *cwist_test_client_post_json(cwist_test_client *client, const char *path, const char *json_body);

/**
 * @brief Perform a PUT request.
 */
cwist_http_response *cwist_test_client_put(cwist_test_client *client, const char *path, const char *body);

/**
 * @brief Perform a DELETE request.
 */
cwist_http_response *cwist_test_client_delete(cwist_test_client *client, const char *path);

/**
 * @brief Perform a PATCH request.
 */
cwist_http_response *cwist_test_client_patch(cwist_test_client *client, const char *path, const char *body);

/**
 * @brief Perform a request with full control over headers, cookies and query.
 */
cwist_http_response *cwist_test_client_request_ex(cwist_test_client *client,
                                                   cwist_http_method_t method,
                                                   const char *path,
                                                   const cwist_test_client_request_options *opts);

/**
 * @brief Perform a POST request with multipart/form-data body simulating a file upload.
 *
 * @param field_name Form field name.
 * @param file_name  File name to report.
 * @param content_type MIME type, e.g. "text/plain".
 * @param data       File contents.
 * @param data_len   Length of file contents.
 */
cwist_http_response *cwist_test_client_post_multipart(cwist_test_client *client,
                                                       const char *path,
                                                       const char *field_name,
                                                       const char *file_name,
                                                       const char *content_type,
                                                       const char *data,
                                                       size_t data_len);

/** @} */

/** @name Cookie Jar */
/** @{ */

/**
 * @brief Set a persistent cookie in the client's cookie jar.
 */
void cwist_test_client_set_cookie(cwist_test_client *client,
                                   const char *name,
                                   const char *value,
                                   const char *path);

/**
 * @brief Get a cookie value from the jar.
 * @return Cookie value or NULL if not found. Do not free.
 */
const char *cwist_test_client_get_cookie(cwist_test_client *client, const char *name);

/**
 * @brief Clear all cookies in the jar.
 */
void cwist_test_client_clear_cookies(cwist_test_client *client);

/** @} */

#endif
