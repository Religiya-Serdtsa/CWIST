#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/test_client.h>
#include <cwist/net/http/multipart.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#define FAIL_IF(cond, msg) do { if (cond) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "hello");
    res->status_code = CWIST_HTTP_OK;
}

static void echo_handler(cwist_http_request *req, cwist_http_response *res) {
    if (req->body && req->body->data) cwist_sstring_assign(res->body, req->body->data);
    res->status_code = CWIST_HTTP_OK;
}

static void put_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "put");
    res->status_code = CWIST_HTTP_OK;
}

static void delete_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "delete");
    res->status_code = CWIST_HTTP_OK;
}

static void patch_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "patch");
    res->status_code = CWIST_HTTP_OK;
}

static void set_cookie_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_header_add(&res->headers, "Set-Cookie", "tc=jarvalue; Path=/");
    cwist_sstring_assign(res->body, "ok");
    res->status_code = CWIST_HTTP_OK;
}

static void get_cookie_handler(cwist_http_request *req, cwist_http_response *res) {
    const char *cookie = cwist_http_header_get(req->headers, "Cookie");
    if (cookie) cwist_sstring_assign_len(res->body, cookie, strlen(cookie));
    else cwist_sstring_assign(res->body, "none");
    res->status_code = CWIST_HTTP_OK;
}

static void query_handler(cwist_http_request *req, cwist_http_response *res) {
    const char *q = cwist_query_map_get(req->query_params, "q");
    if (q) cwist_sstring_assign_len(res->body, q, strlen(q));
    else cwist_sstring_assign(res->body, "missing");
    res->status_code = CWIST_HTTP_OK;
}

static void upload_handler(cwist_http_request *req, cwist_http_response *res) {
    const char *ct = cwist_http_header_get(req->headers, "Content-Type");
    if (!ct) { res->status_code = CWIST_HTTP_BAD_REQUEST; return; }
    char *boundary = cwist_multipart_extract_boundary(ct);
    if (!boundary) { res->status_code = CWIST_HTTP_BAD_REQUEST; return; }
    cwist_multipart_result *mr = cwist_multipart_parse(req->body->data, req->body->size, boundary);
    free(boundary);
    if (!mr || !mr->fields) { res->status_code = CWIST_HTTP_BAD_REQUEST; return; }
    cwist_sstring_assign(res->body, mr->fields->filename ? mr->fields->filename : "no-filename");
    cwist_sstring_append(res->body, "=");
    if (mr->fields->data) cwist_sstring_append(res->body, mr->fields->data);
    cwist_multipart_result_destroy(mr);
    res->status_code = CWIST_HTTP_OK;
}

int main(void) {
    cwist_app *app = cwist_app_create();
    FAIL_IF(!app, "app_create");

    cwist_app_get(app, "/", hello_handler);
    cwist_app_post(app, "/echo", echo_handler);
    cwist_app_put(app, "/resource", put_handler);
    cwist_app_delete(app, "/resource", delete_handler);
    cwist_app_patch(app, "/resource", patch_handler);
    cwist_app_get(app, "/set-cookie", set_cookie_handler);
    cwist_app_get(app, "/get-cookie", get_cookie_handler);
    cwist_app_get(app, "/query", query_handler);
    cwist_app_post(app, "/upload", upload_handler);

    cwist_test_client *client = cwist_test_client_create(app);
    FAIL_IF(!client, "client_create");

    /* GET */
    cwist_http_response *res = cwist_test_client_get(client, "/");
    FAIL_IF(!res || !res->body, "get res");
    FAIL_IF(strcmp(res->body->data, "hello") != 0, "get body");
    cwist_http_response_destroy(res);

    /* POST */
    res = cwist_test_client_post(client, "/echo", "payload");
    FAIL_IF(!res || !res->body, "post res");
    FAIL_IF(strcmp(res->body->data, "payload") != 0, "post body");
    cwist_http_response_destroy(res);

    /* POST JSON */
    res = cwist_test_client_post_json(client, "/echo", "{\"x\":1}");
    FAIL_IF(!res || !res->body, "post_json res");
    FAIL_IF(strcmp(res->body->data, "{\"x\":1}") != 0, "post_json body");
    cwist_http_response_destroy(res);

    /* PUT */
    res = cwist_test_client_put(client, "/resource", "data");
    FAIL_IF(!res || !res->body, "put res");
    FAIL_IF(strcmp(res->body->data, "put") != 0, "put body");
    cwist_http_response_destroy(res);

    /* DELETE */
    res = cwist_test_client_delete(client, "/resource");
    FAIL_IF(!res || !res->body, "delete res");
    FAIL_IF(strcmp(res->body->data, "delete") != 0, "delete body");
    cwist_http_response_destroy(res);

    /* PATCH */
    res = cwist_test_client_patch(client, "/resource", "data");
    FAIL_IF(!res || !res->body, "patch res");
    FAIL_IF(strcmp(res->body->data, "patch") != 0, "patch body");
    cwist_http_response_destroy(res);

    /* request_ex with headers, query, ad-hoc cookies */
    cwist_test_client_kv headers[] = { {"X-Custom", "abc"} };
    cwist_test_client_kv cookies[] = { {"adhoc", "123"} };
    cwist_test_client_request_options opts = {
        .headers = headers,
        .header_count = 1,
        .cookies = cookies,
        .cookie_count = 1,
        .query_string = "q=search"
    };
    res = cwist_test_client_request_ex(client, CWIST_HTTP_GET, "/query", &opts);
    FAIL_IF(!res || !res->body, "request_ex res");
    FAIL_IF(strcmp(res->body->data, "search") != 0, "query body");
    cwist_http_response_destroy(res);

    /* Cookie jar: set, send, receive from response, send again */
    cwist_test_client_set_cookie(client, "manual", "mv", "/");
    FAIL_IF(strcmp(cwist_test_client_get_cookie(client, "manual"), "mv") != 0, "manual cookie");

    res = cwist_test_client_get(client, "/set-cookie");
    FAIL_IF(!res || strcmp(cwist_test_client_get_cookie(client, "tc"), "jarvalue") != 0, "jar cookie");
    cwist_http_response_destroy(res);

    res = cwist_test_client_get(client, "/get-cookie");
    FAIL_IF(!res || !res->body, "get-cookie res");
    FAIL_IF(strstr(res->body->data, "tc=jarvalue") == NULL, "jar cookie sent");
    FAIL_IF(strstr(res->body->data, "manual=mv") == NULL, "manual cookie sent");
    cwist_http_response_destroy(res);

    /* Multipart upload */
    res = cwist_test_client_post_multipart(client, "/upload", "file", "test.txt",
                                            "text/plain", "hello file", 10);
    FAIL_IF(!res || !res->body, "multipart res");
    if (!res->body->data) {
        fprintf(stderr, "multipart body->data is NULL, status=%d\n", res->status_code);
        return 1;
    }
    FAIL_IF(strstr(res->body->data, "test.txt=hello file") == NULL, "multipart body");
    cwist_http_response_destroy(res);

    /* Path with query string */
    res = cwist_test_client_get(client, "/query?q=direct");
    FAIL_IF(!res || !res->body, "path query res");
    FAIL_IF(strcmp(res->body->data, "direct") != 0, "path query body");
    cwist_http_response_destroy(res);

    cwist_test_client_destroy(client);
    cwist_app_destroy(app);

    printf("test_test_client: OK\n");
    return 0;
}
