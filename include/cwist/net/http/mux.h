#ifndef __CWIST_MUX_H__
#define __CWIST_MUX_H__

#include <cwist/net/http/http.h>
#include <cwist/sys/app/app.h>
#include <cwist/core/sstring/sstring.h>
#include <stdint.h>

/** --- Mux Router --- */

typedef void (*cwist_http_handler_func)(cwist_http_request *req, cwist_http_response *res);

typedef struct cwist_mux_middleware_node {
    cwist_middleware_func func;
    struct cwist_mux_middleware_node *next;
} cwist_mux_middleware_node;

typedef struct cwist_mux_route {
    cwist_http_method_t method;
    cwist_sstring *path;
    cwist_http_handler_func handler;
    uint64_t signature_hi;
    uint64_t signature_lo;
    bool is_parametric; ///< True if path contains parameters like :id
    bool is_wildcard;   ///< True if path ends with *
    cwist_mux_middleware_node *middleware; ///< Per-route middleware chain
    struct cwist_mux_route *bucket_next; ///< Used for exact matches
    struct cwist_mux_route *param_next;  ///< Used for parametric routes
    struct cwist_mux_route *next;        ///< Used for global cleanup
} cwist_mux_route;

typedef struct cwist_mux_router {
    size_t bucket_count;
    cwist_mux_route **buckets;
    cwist_mux_route *routes;
    cwist_mux_route *param_routes; ///< Head of the parametric routes list
    cwist_mux_route *wildcard_routes; ///< Head of wildcard routes list
} cwist_mux_router;

typedef struct cwist_mux_group {
    cwist_mux_router *router;
    char *prefix;
} cwist_mux_group;

/** @name Lifecycle */
/** @{ */
cwist_mux_router *cwist_mux_router_create(void);
void cwist_mux_router_destroy(cwist_mux_router *router);
/** @} */

/** @name Route Management */
/** @{ */
void cwist_mux_handle(cwist_mux_router *router, cwist_http_method_t method, const char *path, cwist_http_handler_func handler);
cwist_mux_route *cwist_mux_find_route(cwist_mux_router *router, cwist_http_method_t method, const char *path);
/** @} */

/** @name Route Groups */
/** @{ */
cwist_mux_group *cwist_mux_group_create(cwist_mux_router *router, const char *prefix);
void cwist_mux_group_destroy(cwist_mux_group *group);
void cwist_mux_group_handle(cwist_mux_group *group, cwist_http_method_t method, const char *path, cwist_http_handler_func handler);
/** @} */

/** @name Per-Route Middleware */
/** @{ */
void cwist_mux_route_use(cwist_mux_route *route, cwist_middleware_func mw);
/** @} */

/** @name Dispatch */
/** @{ */
/**
 * @brief Dispatches an HTTP request through the router.
 * @return True if a route was found and executed, false otherwise (404).
 */
bool cwist_mux_serve(cwist_mux_router *router, cwist_http_request *req, cwist_http_response *res);
/** @} */

#endif
