#include <cwist/net/http/mux.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/query.h>
#include <cwist/core/sstring/sstring.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

void test_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, "Handled!");
}

int main() {
    printf("Running Mux & Query Tests...\n");

    /* --- Test Query Parsing --- */
    printf("1. Testing Query Parsing... ");
    cwist_query_map *map = cwist_query_map_create();
    cwist_query_map_parse(map, "name=yjlee&role=admin&active");
    
    const char *val_name = cwist_query_map_get(map, "name");
    const char *val_role = cwist_query_map_get(map, "role");
    const char *val_active = cwist_query_map_get(map, "active");
    const char *val_missing = cwist_query_map_get(map, "missing");

    assert(val_name && strcmp(val_name, "yjlee") == 0);
    assert(val_role && strcmp(val_role, "admin") == 0);
    assert(val_active && strcmp(val_active, "") == 0);
    assert(val_missing == NULL);

    cwist_query_map_destroy(map);
    printf("PASSED\n");

    /* --- Test Mux Router --- */
    printf("2. Testing Mux Router... ");
    cwist_mux_router *router = cwist_mux_router_create();
    cwist_mux_handle(router, CWIST_HTTP_GET, "/test", test_handler);

    // Mock Request 1: Match
    cwist_http_request *req1 = cwist_http_request_create();
    req1->method = CWIST_HTTP_GET;
    cwist_sstring_assign(req1->path, "/test");

    cwist_http_response *res1 = cwist_http_response_create();
    
    bool handled1 = cwist_mux_serve(router, req1, res1);
    assert(handled1 == true);
    assert(res1->status_code == CWIST_HTTP_OK);
    assert(strcmp(res1->body->data, "Handled!") == 0);

    // Mock Request 2: No Match (Path)
    cwist_http_request *req2 = cwist_http_request_create();
    req2->method = CWIST_HTTP_GET;
    cwist_sstring_assign(req2->path, "/other");
    cwist_http_response *res2 = cwist_http_response_create();
    bool handled2 = cwist_mux_serve(router, req2, res2);
    assert(handled2 == false);

    // Mock Request 3: No Match (Method)
    cwist_http_request *req3 = cwist_http_request_create();
    req3->method = CWIST_HTTP_POST;
    cwist_sstring_assign(req3->path, "/test");
    cwist_http_response *res3 = cwist_http_response_create();
    bool handled3 = cwist_mux_serve(router, req3, res3);
    assert(handled3 == false);

    cwist_mux_router_destroy(router);
    cwist_http_request_destroy(req1);
    cwist_http_response_destroy(res1);
    cwist_http_request_destroy(req2);
    cwist_http_response_destroy(res2);
    cwist_http_request_destroy(req3);
    cwist_http_response_destroy(res3);

    printf("PASSED\n");

    /* --- Test Integration (Parse Request with Query) --- */
    printf("3. Testing Integration... ");
    const char *raw_req = "GET /api?foo=bar&baz=123 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    cwist_http_request *parsed_req = cwist_http_parse_request(raw_req);
    
    assert(parsed_req != NULL);
    assert(strcmp(parsed_req->path->data, "/api") == 0);
    assert(strcmp(parsed_req->query->data, "foo=bar&baz=123") == 0);
    
    const char *q_foo = cwist_query_map_get(parsed_req->query_params, "foo");
    const char *q_baz = cwist_query_map_get(parsed_req->query_params, "baz");

    assert(q_foo && strcmp(q_foo, "bar") == 0);
    assert(q_baz && strcmp(q_baz, "123") == 0);

    cwist_http_request_destroy(parsed_req);
    printf("PASSED\n");

    return 0;
}
