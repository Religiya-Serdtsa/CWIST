/**
 * @file test_client.h
 * @brief In-process HTTP test client (ala Flask test_client).
 */

#ifndef __CWIST_TEST_CLIENT_H__
#define __CWIST_TEST_CLIENT_H__

#include <cwist/sys/app/app.h>

typedef struct cwist_test_client {
    cwist_app *app;
} cwist_test_client;

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

/** @} */

#endif
