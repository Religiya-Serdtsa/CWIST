#include <cwist/net/http/http.h>
#include <cwist/sys/app/middleware.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

static bool next_called = false;

void dummy_next(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    (void)res;
    next_called = true;
}

void test_cors_options() {
    printf("Testing CORS OPTIONS (Preflight)...\n");
    cwist_middleware_func cors = cwist_mw_cors();
    
    cwist_http_request *req = cwist_http_request_create();
    req->method = CWIST_HTTP_OPTIONS;
    
    cwist_http_response *res = cwist_http_response_create();
    
    next_called = false;
    cors(req, res, dummy_next);
    
    // Check short-circuit
    assert(next_called == false);
    
    // Check Status
    assert(res->status_code == CWIST_HTTP_NO_CONTENT);
    
    // Check Headers
    assert(strcmp(cwist_http_header_get(res->headers, "Access-Control-Allow-Origin"), "*") == 0);
    assert(cwist_http_header_get(res->headers, "Access-Control-Allow-Methods") != NULL);
    assert(cwist_http_header_get(res->headers, "Access-Control-Allow-Headers") != NULL);
    assert(cwist_http_header_get(res->headers, "Access-Control-Max-Age") != NULL);
    
    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    printf("Passed CORS OPTIONS.\n");
}

void test_cors_simple() {
    printf("Testing CORS Simple Request (GET)...\n");
    cwist_middleware_func cors = cwist_mw_cors();
    
    cwist_http_request *req = cwist_http_request_create();
    req->method = CWIST_HTTP_GET;
    
    cwist_http_response *res = cwist_http_response_create();
    
    next_called = false;
    cors(req, res, dummy_next);
    
    // Check next called
    assert(next_called == true);
    
    // Check Headers
    assert(strcmp(cwist_http_header_get(res->headers, "Access-Control-Allow-Origin"), "*") == 0);
    // Should NOT have preflight headers
    assert(cwist_http_header_get(res->headers, "Access-Control-Allow-Methods") == NULL);
    
    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    printf("Passed CORS Simple Request.\n");
}

int main() {
    test_cors_options();
    test_cors_simple();
    printf("All CORS tests passed!\n");
    return 0;
}
