#define _POSIX_C_SOURCE 200809L
/* In-memory dispatch: raw HTTP/1.x request buffer in, serialized HTTP
 * response buffer out — no sockets, no threads. */
#include <cwist/sys/app/app.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/utils/json_builder.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "hello-memory");
}

static void json_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_json_builder *jb = cwist_json_builder_create();
    assert(jb != NULL);
    cwist_json_begin_object(jb);
    cwist_json_add_string(jb, "method", cwist_http_method_to_string(req->method));
    cwist_json_add_int(jb, "body_len", (int)(req->body ? req->body->size : 0));
    cwist_json_end_object(jb);
    cwist_sstring_assign(res->body, cwist_json_get_raw(jb));
    cwist_json_builder_destroy(jb);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    res->status_code = CWIST_HTTP_CREATED;
}

static void mw_marker(cwist_http_request *req, cwist_http_response *res,
                      cwist_handler_func next) {
    cwist_http_header_add(&res->headers, "X-Mw", "seen");
    next(req, res);
}

static void assert_dispatches(cwist_app *app, const char *req, size_t req_len,
                              const char *status_line, const char *header,
                              const char *body) {
    char *res_buf = NULL;
    size_t res_len = 0;
    assert(cwist_app_dispatch_memory(app, req, req_len, &res_buf, &res_len) == 0);
    assert(res_buf != NULL && res_len > 0);
    assert(res_len == strlen(res_buf)); /* no truncation at NUL */

    assert(strncmp(res_buf, status_line, strlen(status_line)) == 0);
    if (header) assert(strstr(res_buf, header) != NULL);
    assert(strstr(res_buf, "\r\nConnection: close\r\n") != NULL);
    if (body) {
        const char *body_start = strstr(res_buf, "\r\n\r\n");
        assert(body_start != NULL);
        body_start += 4;
        assert(res_len - (size_t)(body_start - res_buf) == strlen(body));
        assert(memcmp(body_start, body, strlen(body)) == 0);
    }
    cwist_free(res_buf);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    cwist_app_use(app, mw_marker);
    cwist_app_get(app, "/hello", hello_handler);
    cwist_app_post(app, "/json", json_handler);

    /* Static string route through the middleware chain. */
    static const char get_req[] =
        "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert_dispatches(app, get_req, sizeof(get_req) - 1,
                      "HTTP/1.1 200 OK\r\n", "X-Mw: seen", "hello-memory");

    /* POST with a binary-safe body (contains a NUL) via Content-Length. */
    static const char post_head[] =
        "POST /json HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/octet-stream\r\nContent-Length: 5\r\n\r\n";
    char post_req[sizeof(post_head) + 5];
    memcpy(post_req, post_head, sizeof(post_head) - 1);
    memcpy(post_req + sizeof(post_head) - 1, "a\0bcd", 5);
    assert_dispatches(app, post_req, sizeof(post_head) - 1 + 5,
                      "HTTP/1.1 201 Created\r\n", "Content-Type: application/json",
                      "{\"method\":\"POST\",\"body_len\":5}");

    /* Unknown path hits the 404 error path (which bypasses route middleware,
     * matching the socket transport behavior). */
    static const char missing_req[] =
        "GET /nope HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert_dispatches(app, missing_req, sizeof(missing_req) - 1,
                      "HTTP/1.1 404", NULL, NULL);

    /* Malformed request is rejected without a response buffer. */
    char *res_buf = NULL;
    size_t res_len = 0;
    assert(cwist_app_dispatch_memory(app, "garbage", 7, &res_buf, &res_len) == -1);
    assert(res_buf == NULL && res_len == 0);
    assert(cwist_app_dispatch_memory(NULL, get_req, sizeof(get_req) - 1,
                                     &res_buf, &res_len) == -1);

    cwist_app_destroy(app);
    printf("test_dispatch_memory: OK\n");
    return 0;
}
