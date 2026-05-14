/** @file mux.h
 * @brief mux.h interface.
 */
#ifndef __CWIST_MUX_H__
#define __CWIST_MUX_H__

#include <cwist/http.h>
#include <cwist/sstring.h>

/** --- Mux Router --- */

typedef void (*cwist_http_handler_func)(cwist_http_request *req, cwist_http_response *res);

typedef struct cwist_mux_route {
    cwist_http_method_t method;
    cwist_sstring *path;
    cwist_http_handler_func handler;
    struct cwist_mux_route *next;
} cwist_mux_route;

typedef struct cwist_mux_router {
    cwist_mux_route *routes;
} cwist_mux_router;

/** @name Lifecycle */
/** @{ */

/**
 * @brief Create a new router.
 */
cwist_mux_router *cwist_mux_router_create(void);

/**
 * @brief Destroy a router.
 */
void cwist_mux_router_destroy(cwist_mux_router *router);

/** @} */

/** @name Route Management */
/** @{ */

/**
 * @brief Register a route handler.
 */
void cwist_mux_handle(cwist_mux_router *router, cwist_http_method_t method, const char *path, cwist_http_handler_func handler);

/** @} */

/** @name Dispatch */
/** @{ */

/**
 * @brief Dispatch a request.
 * @return True if a route was found and executed, false otherwise (404).
 */
bool cwist_mux_serve(cwist_mux_router *router, cwist_http_request *req, cwist_http_response *res);

/** @} */

#endif
