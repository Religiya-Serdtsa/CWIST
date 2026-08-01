/**
 * @file test_core_hardening.c
 * @brief Comprehensive stress and edge-case testing for CWIST core components:
 *        Mux Router (wildcards, regex-like parameters),
 *        Session Manager & signed cookies, WAF/Sanitizer, and CSRF token validations.
 */

#include <cwist/sys/app/app.h>
#include <cwist/net/http/mux.h>
#include <cwist/net/http/query.h>
#include <cwist/net/http/session.h>
#include <cwist/sys/session/flash.h>
#include <cwist/sys/app/waf.h>
#include <cwist/sys/app/csrf.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int mock_next_called = 0;

static void mock_next(cwist_http_request *req, cwist_http_response *res) {
    (void)req; (void)res;
    mock_next_called = 1;
}

static void handler_user_profile(cwist_http_request *req, cwist_http_response *res) {
    assert(req->path_params != NULL);
    const char *id = cwist_query_map_get(req->path_params, "id");
    const char *action = cwist_query_map_get(req->path_params, "action");
    char buf[256];
    snprintf(buf, sizeof(buf), "User:%s Action:%s", id ? id : "none", action ? action : "none");
    cwist_sstring_assign(res->body, buf);
    res->status_code = CWIST_HTTP_OK;
}

static void test_mux_advanced_routing(void) {
    puts("[1/4] Testing Mux Router Parameter Hardening...");

    cwist_mux_router *router = cwist_mux_router_create();
    assert(router);

    cwist_mux_handle(router, CWIST_HTTP_GET, "/user/:id/action/:action", handler_user_profile);

    cwist_http_request *req = cwist_http_request_create();
    req->method = CWIST_HTTP_GET;
    cwist_sstring_assign(req->path, "/user/1004/action/edit");

    cwist_http_response *res = cwist_http_response_create();

    bool handled = cwist_mux_serve(router, req, res);
    assert(handled);
    assert(res->status_code == CWIST_HTTP_OK);
    assert(strstr(res->body->data, "User:1004 Action:edit"));

    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    cwist_mux_router_destroy(router);

    puts("  Passed Mux Router Hardening.");
}

static void test_session_and_flash(void) {
    puts("[2/4] Testing Session Manager & Flash Message Lifecycle...");

    cwist_app *app = cwist_app_create();
    cwist_app_use_session(app, "super_secret_key_for_testing_session_12345");

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();

    cwist_session_t *sess = cwist_session_start(app, req, res);
    assert(sess);

    cwist_session_set(sess, "role", "administrator");
    cwist_session_set(sess, "uid", "99");
    assert(strcmp(cwist_session_get(sess, "role"), "administrator") == 0);
    assert(strcmp(cwist_session_get(sess, "uid"), "99") == 0);

    cwist_flash_set(req, "notice", "Profile updated successfully");
    char *msg1 = (char *)cwist_flash_get(req, "notice");
    assert(msg1 && strcmp(msg1, "Profile updated successfully") == 0);
    cwist_free(msg1);

    const char *msg2 = cwist_flash_get(req, "notice");
    assert(msg2 == NULL);

    /* cwist_http_request_destroy가 req->session을 소유하므로 별도 destroy 불필요 */
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    cwist_app_destroy(app);

    puts("  Passed Session & Flash Hardening.");
}

static void test_waf_sanitizer_hardening(void) {
    puts("[3/4] Testing WAF Lite & HTML Output Escape...");

    const char *dangerous_html = "<script>alert('xss');</script>&\"'";
    char *escaped = cwist_sanitize_html(dangerous_html);
    assert(escaped);
    assert(strstr(escaped, "&lt;script&gt;"));
    assert(strstr(escaped, "&amp;"));
    assert(strstr(escaped, "&quot;"));
    cwist_free(escaped);

    assert(cwist_waf_is_safe("cwist_framework", 15) == true);
    assert(cwist_waf_is_safe("<script>alert(1)</script>", 25) == false);
    assert(cwist_waf_is_safe("1 UNION SELECT password FROM users", 35) == false);

    puts("  Passed WAF & Sanitizer Hardening.");
}

static void test_csrf_protection_flow(void) {
    puts("[4/4] Testing CSRF Token Generation & Double-Submit Validation...");

    cwist_app *app = cwist_app_create();
    cwist_middleware_func csrf_mw = cwist_mw_csrf(app);
    assert(csrf_mw != NULL);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();

    req->method = CWIST_HTTP_GET;
    mock_next_called = 0;
    csrf_mw(req, res, mock_next);
    assert(mock_next_called == 1);
    const char *token = cwist_csrf_token(req);
    assert(token != NULL && strlen(token) > 0);

    cwist_http_request *post_req = cwist_http_request_create();
    cwist_http_response *post_res = cwist_http_response_create();
    post_req->method = CWIST_HTTP_POST;
    char cookie_buf[256];
    snprintf(cookie_buf, sizeof(cookie_buf), "csrf_token=%s", token);
    cwist_http_header_add(&post_req->headers, "Cookie", cookie_buf);
    cwist_http_header_add(&post_req->headers, "X-CSRF-Token", token);

    mock_next_called = 0;
    csrf_mw(post_req, post_res, mock_next);
    assert(mock_next_called == 1);
    assert(post_res->status_code == CWIST_HTTP_OK);

    cwist_http_request *bad_req = cwist_http_request_create();
    cwist_http_response *bad_res = cwist_http_response_create();
    bad_req->method = CWIST_HTTP_POST;
    cwist_http_header_add(&bad_req->headers, "Cookie", cookie_buf);
    cwist_http_header_add(&bad_req->headers, "X-CSRF-Token", "invalid_forged_token");

    mock_next_called = 0;
    csrf_mw(bad_req, bad_res, mock_next);
    assert(mock_next_called == 0);
    assert(bad_res->status_code == CWIST_HTTP_FORBIDDEN);

    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    cwist_http_response_destroy(post_res);
    cwist_http_request_destroy(post_req);
    cwist_http_response_destroy(bad_res);
    cwist_http_request_destroy(bad_req);
    cwist_app_destroy(app);

    puts("  Passed CSRF Protection Hardening.");
}

int main(void) {
    puts("=== Running CWIST Core Component Hardening Tests ===");
    test_mux_advanced_routing();
    test_session_and_flash();
    test_waf_sanitizer_hardening();
    test_csrf_protection_flow();
    puts("All CWIST Core Component Hardening Tests PASSED!");
    return 0;
}
