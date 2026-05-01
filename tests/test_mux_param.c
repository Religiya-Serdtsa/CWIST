#include <cwist/net/http/mux.h>
#include <cwist/net/http/query.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void dummy_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    (void)res;
}

static void parameterized_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)res;
    assert(req->path_params != NULL);
    const char *id = cwist_query_map_get(req->path_params, "id");
    const char *post_id = cwist_query_map_get(req->path_params, "post_id");
    assert(id != NULL && strcmp(id, "123") == 0);
    assert(post_id != NULL && strcmp(post_id, "456") == 0);
}

int main(void) {
    printf("Testing Gorilla/Mux style Parametric Router...\n");

    cwist_mux_router *router = cwist_mux_router_create();
    
    // Register exact and parameterized routes
    cwist_mux_handle(router, CWIST_HTTP_GET, "/users", dummy_handler);
    cwist_mux_handle(router, CWIST_HTTP_GET, "/users/:id/posts/:post_id", parameterized_handler);
    
    // Test Exact Match
    cwist_http_request *req1 = cwist_http_request_create();
    req1->method = CWIST_HTTP_GET;
    cwist_sstring_assign(req1->path, "/users");
    cwist_http_response *res1 = cwist_http_response_create();
    assert(cwist_mux_serve(router, req1, res1) == true);
    cwist_http_request_destroy(req1);
    cwist_http_response_destroy(res1);

    // Test Parametric Match
    cwist_http_request *req2 = cwist_http_request_create();
    req2->method = CWIST_HTTP_GET;
    cwist_sstring_assign(req2->path, "/users/123/posts/456");
    cwist_http_response *res2 = cwist_http_response_create();
    assert(cwist_mux_serve(router, req2, res2) == true);
    cwist_http_request_destroy(req2);
    cwist_http_response_destroy(res2);

    // Test Miss
    cwist_http_request *req3 = cwist_http_request_create();
    req3->method = CWIST_HTTP_GET;
    cwist_sstring_assign(req3->path, "/users/123/posts/456/extra");
    cwist_http_response *res3 = cwist_http_response_create();
    assert(cwist_mux_serve(router, req3, res3) == false);
    cwist_http_request_destroy(req3);
    cwist_http_response_destroy(res3);

    cwist_mux_router_destroy(router);
    printf("All Mux tests passed!\n");

    return 0;
}
