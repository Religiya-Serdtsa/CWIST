/** @file test_client.c
 * @brief test_client.c interface.
 */
#include <cwist/sys/app/test_client.h>
#include <cwist/core/mem/alloc.h>
#include <string.h>

cwist_test_client *cwist_test_client_create(cwist_app *app) {
    if (!app) return NULL;
    cwist_test_client *client = (cwist_test_client *)cwist_alloc(sizeof(cwist_test_client));
    if (!client) return NULL;
    client->app = app;
    return client;
}

void cwist_test_client_destroy(cwist_test_client *client) {
    cwist_free(client);
}

cwist_http_response *cwist_test_client_get(cwist_test_client *client, const char *path) {
    if (!client || !client->app || !path) return NULL;
    cwist_http_request *req = cwist_http_request_create();
    if (!req) return NULL;
    req->method = CWIST_HTTP_GET;
    cwist_sstring_assign(req->path, (char *)path);
    cwist_http_response *res = cwist_http_response_create();
    if (!res) {
        cwist_http_request_destroy(req);
        return NULL;
    }
    cwist_app_dispatch(client->app, req, res);
    cwist_http_request_destroy(req);
    return res;
}

cwist_http_response *cwist_test_client_post(cwist_test_client *client, const char *path, const char *body) {
    if (!client || !client->app || !path) return NULL;
    cwist_http_request *req = cwist_http_request_create();
    if (!req) return NULL;
    req->method = CWIST_HTTP_POST;
    cwist_sstring_assign(req->path, (char *)path);
    if (body) {
        cwist_sstring_assign(req->body, (char *)body);
    }
    cwist_http_response *res = cwist_http_response_create();
    if (!res) {
        cwist_http_request_destroy(req);
        return NULL;
    }
    cwist_app_dispatch(client->app, req, res);
    cwist_http_request_destroy(req);
    return res;
}

cwist_http_response *cwist_test_client_post_json(cwist_test_client *client, const char *path, const char *json_body) {
    if (!client || !client->app || !path) return NULL;
    cwist_http_request *req = cwist_http_request_create();
    if (!req) return NULL;
    req->method = CWIST_HTTP_POST;
    cwist_sstring_assign(req->path, (char *)path);
    if (json_body) {
        cwist_sstring_assign(req->body, (char *)json_body);
    }
    cwist_http_header_add(&req->headers, "Content-Type", "application/json");
    cwist_http_response *res = cwist_http_response_create();
    if (!res) {
        cwist_http_request_destroy(req);
        return NULL;
    }
    cwist_app_dispatch(client->app, req, res);
    cwist_http_request_destroy(req);
    return res;
}
