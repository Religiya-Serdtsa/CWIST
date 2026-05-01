#ifndef __CWIST_TEST_CLIENT_H__
#define __CWIST_TEST_CLIENT_H__

/**
 * @file test_client.h
 * @brief In-process HTTP test client (ala Flask test_client).
 */

#include <cwist/sys/app/app.h>

typedef struct cwist_test_client {
    cwist_app *app;
} cwist_test_client;

/** @name Lifecycle */
/** @{ */
cwist_test_client *cwist_test_client_create(cwist_app *app);
void cwist_test_client_destroy(cwist_test_client *client);
/** @} */

/** @name HTTP Methods */
/** @{ */
cwist_http_response *cwist_test_client_get(cwist_test_client *client, const char *path);
cwist_http_response *cwist_test_client_post(cwist_test_client *client, const char *path, const char *body);
cwist_http_response *cwist_test_client_post_json(cwist_test_client *client, const char *path, const char *json_body);
/** @} */

#endif
