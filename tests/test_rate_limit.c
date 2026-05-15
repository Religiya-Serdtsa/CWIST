/**
 * @file test_rate_limit.c
 * @brief Unit tests for token-bucket rate limiting middleware.
 */

#include <cwist/sys/app/middleware.h>
#include <cwist/net/http/http.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

static void dummy_next(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_OK;
}

void test_rate_limit_blocks_after_burst(void) {
    printf("test_rate_limit_blocks_after_burst...\n");
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->client_fd = -1;

    cwist_middleware_func mw = cwist_mw_rate_limit_ip(2); /* 2 req/min */
    mw(req, res, dummy_next);
    assert(res->status_code == CWIST_HTTP_OK);
    mw(req, res, dummy_next);
    assert(res->status_code == CWIST_HTTP_OK);
    mw(req, res, dummy_next); /* 3rd should be blocked */
    assert(res->status_code == 429);

    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    cwist_mw_rate_limit_reset();
    printf("  OK\n");
}

void test_rate_limit_respects_param(void) {
    printf("test_rate_limit_respects_param...\n");
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->client_fd = -1;

    cwist_middleware_func mw = cwist_mw_rate_limit_ip(1); /* 1 req/min */
    mw(req, res, dummy_next);
    assert(res->status_code == CWIST_HTTP_OK);
    mw(req, res, dummy_next); /* 2nd blocked immediately */
    assert(res->status_code == 429);

    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    printf("  OK\n");
}

int main(void) {
    test_rate_limit_blocks_after_burst();
    test_rate_limit_respects_param();
    printf("All rate limit tests passed.\n");
    return 0;
}
