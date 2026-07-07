#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/csrf.h>
#include <cwist/net/http/http.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int next_called = 0;

static void test_next(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    (void)res;
    next_called = 1;
}

static int response_has_set_cookie(cwist_http_response *res, const char *name) {
    cwist_http_header_node *curr = res->headers;
    while (curr) {
        if (curr->key && curr->value &&
            strcasecmp(curr->key->data, "Set-Cookie") == 0 &&
            strncmp(curr->value->data, name, strlen(name)) == 0) {
            return 1;
        }
        curr = curr->next;
    }
    return 0;
}

static void test_csrf_safe_issues_cookie(void) {
    cwist_app *app = cwist_app_create();
    cwist_middleware_func mw = cwist_mw_csrf(app);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->method = CWIST_HTTP_GET;

    next_called = 0;
    mw(req, res, test_next);

    assert(next_called == 1);
    assert(response_has_set_cookie(res, "csrf_token"));
    assert(req->csrf_token != NULL);

    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    cwist_app_destroy(app);
    printf("[csrf] safe GET issues cookie OK\n");
}

static void test_csrf_unsafe_missing_rejected(void) {
    cwist_app *app = cwist_app_create();
    cwist_middleware_func mw = cwist_mw_csrf(app);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->method = CWIST_HTTP_POST;

    next_called = 0;
    mw(req, res, test_next);

    assert(next_called == 0);
    assert(res->status_code == CWIST_HTTP_FORBIDDEN);

    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    cwist_app_destroy(app);
    printf("[csrf] unsafe without cookie rejected OK\n");
}

static void test_csrf_unsafe_with_token_accepted(void) {
    cwist_app *app = cwist_app_create();
    cwist_middleware_func mw = cwist_mw_csrf(app);

    /* First GET to obtain a token. */
    cwist_http_request *req_get = cwist_http_request_create();
    cwist_http_response *res_get = cwist_http_response_create();
    req_get->method = CWIST_HTTP_GET;
    mw(req_get, res_get, test_next);
    const char *token = cwist_csrf_token(req_get);
    assert(token != NULL);

    /* Now POST with matching token in form body. */
    cwist_http_request *req_post = cwist_http_request_create();
    cwist_http_response *res_post = cwist_http_response_create();
    req_post->method = CWIST_HTTP_POST;

    char cookie_header[256];
    snprintf(cookie_header, sizeof(cookie_header), "csrf_token=%s", token);
    cwist_http_header_add(&req_post->headers, "Cookie", cookie_header);

    char body[512];
    snprintf(body, sizeof(body), "_csrf=%s&foo=bar", token);
    cwist_sstring_assign(req_post->body, body);

    next_called = 0;
    mw(req_post, res_post, test_next);

    assert(next_called == 1);
    assert(res_post->status_code == CWIST_HTTP_OK);

    cwist_http_response_destroy(res_get);
    cwist_http_request_destroy(req_get);
    cwist_http_response_destroy(res_post);
    cwist_http_request_destroy(req_post);
    cwist_app_destroy(app);
    printf("[csrf] unsafe with matching token accepted OK\n");
}

int main(void) {
    test_csrf_safe_issues_cookie();
    test_csrf_unsafe_missing_rejected();
    test_csrf_unsafe_with_token_accepted();
    printf("All CSRF tests passed.\n");
    return 0;
}
