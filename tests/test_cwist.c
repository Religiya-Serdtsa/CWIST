#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <cwist/app.h>
#include <cwist/net/http/mux.h>

static void handle_root(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    if (res && res->body) {
        cwist_sstring_assign(res->body, "Hello, World!");
    }
}

static void test_app_create_and_destroy(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    cwist_app_destroy(app);
    printf("test_app_create_and_destroy: PASSED\n");
}

static void test_app_route_registration(void) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    cwist_app_get(app, "/", handle_root);
    cwist_app_get(app, "/health", handle_root);
    cwist_app_destroy(app);
    printf("test_app_route_registration: PASSED\n");
}

static void test_mux_routing_and_indexing(void) {
    cwist_mux_router *router = cwist_mux_router_create();
    assert(router != NULL);

    cwist_mux_handle(router, CWIST_HTTP_GET, "/api/v1/users", handle_root);
    cwist_mux_handle(router, CWIST_HTTP_GET, "/api/v1/posts", handle_root);
    cwist_mux_handle(router, CWIST_HTTP_GET, "/", handle_root);

    cwist_mux_route *r1 = cwist_mux_find_route(router, CWIST_HTTP_GET, "/api/v1/users");
    assert(r1 != NULL);
    assert(strcmp(r1->path->data, "/api/v1/users") == 0);

    cwist_mux_route *r2 = cwist_mux_find_route(router, CWIST_HTTP_GET, "/api/v1/posts");
    assert(r2 != NULL);
    assert(strcmp(r2->path->data, "/api/v1/posts") == 0);

    cwist_mux_route *r3 = cwist_mux_find_route(router, CWIST_HTTP_GET, "/");
    assert(r3 != NULL);
    assert(strcmp(r3->path->data, "/") == 0);

    /* Verify non-existent route returns NULL */
    cwist_mux_route *r4 = cwist_mux_find_route(router, CWIST_HTTP_GET, "/nonexistent");
    assert(r4 == NULL);

    cwist_mux_router_destroy(router);
    printf("test_mux_routing_and_indexing: PASSED\n");
}

int main(void) {
    printf("=== Running CWIST Test Suite ===\n");
    test_app_create_and_destroy();
    test_app_route_registration();
    test_mux_routing_and_indexing();
    printf("=== All CWIST Tests Passed ===\n");
    return 0;
}
