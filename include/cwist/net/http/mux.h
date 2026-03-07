#ifndef __CWIST_MUX_H__
#define __CWIST_MUX_H__

#include <cwist/net/http/http.h>
#include <cwist/core/sstring/sstring.h>
#include <stdint.h>

/** --- Mux Router --- */

typedef void (*cwist_http_handler_func)(cwist_http_request *req, cwist_http_response *res);

typedef struct cwist_mux_route {
    cwist_http_method_t method;
    cwist_sstring *path;
    cwist_http_handler_func handler;
    uint64_t signature_hi;
    uint64_t signature_lo;
    struct cwist_mux_route *bucket_next;
    struct cwist_mux_route *next;
} cwist_mux_route;

typedef struct cwist_mux_router {
    size_t bucket_count;
    cwist_mux_route **buckets;
    cwist_mux_route *routes;
} cwist_mux_router;

/** @name Lifecycle */
/** @{ */
cwist_mux_router *cwist_mux_router_create(void);
void cwist_mux_router_destroy(cwist_mux_router *router);
/** @} */

/** @name Route Management */
/** @{ */
void cwist_mux_handle(cwist_mux_router *router, cwist_http_method_t method, const char *path, cwist_http_handler_func handler);
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
