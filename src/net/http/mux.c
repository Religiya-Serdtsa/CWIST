#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/mux.h>
#include <cwist/net/http/http.h>
#include <cwist/core/mem/alloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* --- Mux Router Implementation --- */

cwist_mux_router *cwist_mux_router_create(void) {
    cwist_mux_router *router = (cwist_mux_router *)cwist_alloc(sizeof(cwist_mux_router));
    if (!router) return NULL;
    router->routes = NULL;
    return router;
}

void cwist_mux_router_destroy(cwist_mux_router *router) {
    if (!router) return;
    cwist_mux_route *curr = router->routes;
    while (curr) {
        cwist_mux_route *next = curr->next;
        cwist_sstring_destroy(curr->path);
        cwist_free(curr);
        curr = next;
    }
    cwist_free(router);
}

void cwist_mux_handle(cwist_mux_router *router, cwist_http_method_t method, const char *path, cwist_http_handler_func handler) {
    if (!router || !path || !handler) return;

    cwist_mux_route *route = (cwist_mux_route *)cwist_alloc(sizeof(cwist_mux_route));
    if (!route) return;

    route->method = method;
    route->path = cwist_sstring_create();
    cwist_sstring_assign(route->path, (char *)path);
    route->handler = handler;
    
    // Push to front (simplicity)
    route->next = router->routes;
    router->routes = route;
}

bool cwist_mux_serve(cwist_mux_router *router, cwist_http_request *req, cwist_http_response *res) {
    if (!router || !req || !res) return false;

    cwist_mux_route *curr = router->routes;
    while (curr) {
        // Simple exact match for now
        if (curr->method == req->method && 
            curr->path->data && req->path->data &&
            strcmp(curr->path->data, req->path->data) == 0) {
            
            curr->handler(req, res);
            return true;
        }
        curr = curr->next;
    }
    return false;
}
