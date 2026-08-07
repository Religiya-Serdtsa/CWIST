#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/mux.h>
#include <cwist/net/http/http.h>
#include <cwist/core/mem/alloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

/**
 * @file mux.c
 * @brief Fixed-bucket HTTP route dispatch using a lightweight path signature.
 */

#define CWIST_MUX_DEFAULT_BUCKETS 4099

typedef struct {
    uint64_t hi;
    uint64_t lo;
} cwist_mux_signature;

/**
 * @brief Derive the lookup signature for an HTTP method and path pair.
 * @param method HTTP verb associated with the route.
 * @param path Route path to normalise and hash. NULL is treated as the root.
 * @return Two-lane signature suitable for bucket selection and fast equality checks.
 */
static cwist_mux_signature cwist_mux_signature_from_path(cwist_http_method_t method, const char *path) {
    uint64_t hi = 14695981039346656037ULL;
    uint64_t lo = 0xcbf29ce484222325ULL;

    const uint64_t prime_hi = 1099511628211ULL;        /* FNV Prime 64 */
    const uint64_t prime_lo = 0x9e3779b185ebca87ULL;   /* SplitMix Prime */

    /* 1. Method hashing (single pass) */
    const uint8_t *m_ptr = (const uint8_t *)&method;
    for (size_t i = 0; i < sizeof(method); ++i) {
        hi = (hi ^ m_ptr[i]) * prime_hi;
        lo = (lo ^ m_ptr[i]) * prime_lo;
    }

    /* 2. Path normalization and single-pass hashing */
    if (!path) {
        path = "/";
    }

    size_t seg_count = 0;
    const char *cursor = path;

    while (*cursor) {
        while (*cursor == '/') {
            cursor++;
        }
        if (!*cursor) {
            break;
        }

        const char *start = cursor;
        while (*cursor && *cursor != '/') {
            cursor++;
        }

        /* Inject normalized segment delimiter '/' */
        hi = (hi ^ '/') * prime_hi;
        lo = (lo ^ '/') * prime_lo;

        /* Hash segment bytes */
        for (const char *p = start; p < cursor; ++p) {
            uint8_t b = (uint8_t)*p;
            hi = (hi ^ b) * prime_hi;
            lo = (lo ^ b) * prime_lo;
        }
        seg_count++;
    }

    /* Handle root path ('/') or empty path */
    if (seg_count == 0) {
        hi = (hi ^ '/') * prime_hi;
        lo = (lo ^ '/') * prime_lo;
    }

    return (cwist_mux_signature) { .hi = hi, .lo = lo };
}

/**
 * @brief Select the bucket that should hold a specific route signature.
 * @param router Router whose bucket array is being indexed.
 * @param sig Signature produced for the route or request path.
 * @return Stable bucket index in the router's fixed bucket array.
 */
static size_t cwist_mux_bucket_index(const cwist_mux_router *router, const cwist_mux_signature *sig) {
    uint64_t hash = sig->hi ^ (sig->lo + 0x9e3779b97f4a7c15ULL);

    /* SplitMix64-style finalizer to maximize avalanche effect. */
    hash ^= hash >> 30;
    hash *= 0xbf58476d1ce4e5b9ULL;
    hash ^= hash >> 27;
    hash *= 0x94d049bb133111ebULL;
    hash ^= hash >> 31;

    return (size_t)(hash % router->bucket_count);
}

/* --- Mux Router Implementation --- */

/**
 * @brief Create an empty mux router with the default bucket fan-out.
 * @return Newly allocated router, or NULL when allocation fails.
 */
cwist_mux_router *cwist_mux_router_create(void) {
    cwist_mux_router *router = (cwist_mux_router *)cwist_alloc(sizeof(cwist_mux_router));
    if (!router) return NULL;
    router->bucket_count = CWIST_MUX_DEFAULT_BUCKETS;
    router->buckets = (cwist_mux_route **)cwist_alloc_array(router->bucket_count, sizeof(cwist_mux_route *));
    if (!router->buckets) {
        cwist_free(router);
        return NULL;
    }
    memset(router->buckets, 0, router->bucket_count * sizeof(cwist_mux_route *));
    router->routes = NULL;
    router->param_routes = NULL;
    router->wildcard_routes = NULL;
    return router;
}

/**
 * @brief Destroy a mux router and all registered route records.
 * @param router Router to destroy. NULL is ignored.
 */
void cwist_mux_router_destroy(cwist_mux_router *router) {
    if (!router) return;
    cwist_mux_route *curr = router->routes;
    while (curr) {
        cwist_mux_route *next = curr->next;
        cwist_sstring_destroy(curr->path);
        cwist_mux_middleware_node *mw = curr->middleware;
        while (mw) {
            cwist_mux_middleware_node *mw_next = mw->next;
            cwist_free(mw);
            mw = mw_next;
        }
        cwist_free(curr);
        curr = next;
    }
    cwist_free(router->buckets);
    cwist_free(router);
}

/**
 * @brief Register a concrete method/path pair inside the mux buckets.
 * @param router Router that will own the route metadata.
 * @param method HTTP method to match.
 * @param path Exact request path to dispatch.
 * @param handler Callback to invoke for a matching request.
 */
void cwist_mux_handle(cwist_mux_router *router, cwist_http_method_t method, const char *path, cwist_http_handler_func handler) {
    if (!router || !path || !handler) return;

    cwist_mux_route *route = (cwist_mux_route *)cwist_alloc(sizeof(cwist_mux_route));
    if (!route) return;

    route->method = method;
    route->path = cwist_sstring_create();
    cwist_sstring_assign(route->path, (char *)path);
    route->handler = handler;
    route->bucket_next = NULL;
    route->param_next = NULL;
    route->middleware = NULL;
    route->is_wildcard = false;
    route->next = router->routes;
    router->routes = route;

    size_t path_len = strlen(path);
    route->is_parametric = (strchr(path, ':') != NULL);
    route->is_wildcard = (path_len > 0 && path[path_len - 1] == '*');

    if (route->is_wildcard) {
        route->param_next = router->wildcard_routes;
        router->wildcard_routes = route;
    } else if (route->is_parametric) {
        route->param_next = router->param_routes;
        router->param_routes = route;
    } else {
        cwist_mux_signature signature = cwist_mux_signature_from_path(method, path);
        route->signature_hi = signature.hi;
        route->signature_lo = signature.lo;

        size_t idx = cwist_mux_bucket_index(router, &signature);
        route->bucket_next = router->buckets[idx];
        router->buckets[idx] = route;
    }
}

static bool match_parametric_route(const char *route_tmpl, const char *req_path, cwist_query_map **out_params) {
    const char *t = route_tmpl;
    const char *p = req_path;
    cwist_query_map *params = NULL;

    while (*t && *p) {
        if (*t == ':') {
            t++;
            const char *t_next = strchr(t, '/');
            size_t t_len = t_next ? (size_t)(t_next - t) : strlen(t);
            char param_name[256] = {0};
            if (t_len < sizeof(param_name)) memcpy(param_name, t, t_len);

            const char *p_next = strchr(p, '/');
            size_t p_len = p_next ? (size_t)(p_next - p) : strlen(p);
            char param_value[256] = {0};
            if (p_len < sizeof(param_value)) memcpy(param_value, p, p_len);

            if (!params) params = cwist_query_map_create();
            cwist_query_map_set(params, param_name, param_value);

            t += t_len;
            p += p_len;
        } else if (*t == *p) {
            t++;
            p++;
        } else {
            if (params) cwist_query_map_destroy(params);
            return false;
        }
    }

    // Ignore trailing slashes
    if (*t == '/' && *(t + 1) == '\0') t++;
    if (*p == '/' && *(p + 1) == '\0') p++;

    if (*t == '\0' && *p == '\0') {
        *out_params = params;
        return true;
    }

    if (params) cwist_query_map_destroy(params);
    return false;
}

static bool match_wildcard_route(const char *route_tmpl, const char *req_path) {
    size_t len = strlen(route_tmpl);
    if (len > 0 && route_tmpl[len - 1] == '*') {
        size_t prefix_len = len - 1;
        return strncmp(req_path, route_tmpl, prefix_len) == 0;
    }
    return false;
}

static void mux_chain_next(cwist_http_request *req, cwist_http_response *res) {
    typedef struct {
        cwist_mux_middleware_node *current;
        cwist_http_handler_func handler;
    } mux_chain_state;
    mux_chain_state *state = (mux_chain_state *)req->route_middleware_state;
    if (state && state->current) {
        cwist_mux_middleware_node *mw = state->current;
        state->current = mw->next;
        mw->func(req, res, mux_chain_next);
    } else {
        state->handler(req, res);
    }
}

static void run_middleware_chain(cwist_mux_middleware_node *mw, cwist_http_request *req, cwist_http_response *res, cwist_http_handler_func handler) {
    typedef struct {
        cwist_mux_middleware_node *current;
        cwist_http_handler_func handler;
    } mux_chain_state;
    mux_chain_state state = { .current = mw, .handler = handler };
    req->route_middleware_state = &state;
    mux_chain_next(req, res);
    req->route_middleware_state = NULL;
}

cwist_mux_route *cwist_mux_find_route(cwist_mux_router *router, cwist_http_method_t method, const char *path) {
    if (!router || !path) return NULL;
    cwist_mux_signature signature = cwist_mux_signature_from_path(method, path);
    size_t idx = cwist_mux_bucket_index(router, &signature);
    cwist_mux_route *curr = router->buckets[idx];
    while (curr) {
        if (curr->method == method &&
            curr->signature_hi == signature.hi &&
            curr->signature_lo == signature.lo &&
            curr->path && curr->path->data &&
            strcmp(curr->path->data, path) == 0) {
            return curr;
        }
        curr = curr->bucket_next;
    }
    curr = router->param_routes;
    while (curr) {
        if (curr->method == method && curr->path && curr->path->data) {
            cwist_query_map *dummy = NULL;
            if (match_parametric_route(curr->path->data, path, &dummy)) {
                if (dummy) cwist_query_map_destroy(dummy);
                return curr;
            }
        }
        curr = curr->param_next;
    }
    return NULL;
}

/* --- Route Groups --- */

cwist_mux_group *cwist_mux_group_create(cwist_mux_router *router, const char *prefix) {
    if (!router || !prefix) return NULL;
    cwist_mux_group *group = (cwist_mux_group *)cwist_alloc(sizeof(cwist_mux_group));
    if (!group) return NULL;
    group->router = router;
    group->prefix = (char *)cwist_alloc(strlen(prefix) + 1);
    if (!group->prefix) {
        cwist_free(group);
        return NULL;
    }
    strcpy(group->prefix, prefix);
    return group;
}

void cwist_mux_group_destroy(cwist_mux_group *group) {
    if (!group) return;
    cwist_free(group->prefix);
    cwist_free(group);
}

void cwist_mux_group_handle(cwist_mux_group *group, cwist_http_method_t method, const char *path, cwist_http_handler_func handler) {
    if (!group || !group->router || !path || !handler) return;
    size_t prefix_len = strlen(group->prefix);
    size_t path_len = strlen(path);
    char *full_path = (char *)cwist_alloc(prefix_len + path_len + 1);
    if (!full_path) return;
    memcpy(full_path, group->prefix, prefix_len);
    memcpy(full_path + prefix_len, path, path_len);
    full_path[prefix_len + path_len] = '\0';
    cwist_mux_handle(group->router, method, full_path, handler);
    cwist_free(full_path);
}

/* --- Per-Route Middleware --- */

void cwist_mux_route_use(cwist_mux_route *route, cwist_middleware_func mw) {
    if (!route || !mw) return;
    cwist_mux_middleware_node *node = (cwist_mux_middleware_node *)cwist_alloc(sizeof(cwist_mux_middleware_node));
    if (!node) return;
    node->func = mw;
    node->next = route->middleware;
    route->middleware = node;
}

/**
 * @brief Attempt to dispatch an incoming request through the mux table.
 * @param router Router containing registered exact-match handlers.
 * @param req Parsed HTTP request with method and path information.
 * @param res HTTP response object passed through to the matched handler.
 * @return true when a route was found and its handler was executed, otherwise false.
 */
bool cwist_mux_serve(cwist_mux_router *router, cwist_http_request *req, cwist_http_response *res) {
    if (!router || !req || !res) return false;

    const char *path = (req->path && req->path->data) ? req->path->data : "/";
    cwist_mux_signature signature = cwist_mux_signature_from_path(req->method, path);
    size_t idx = cwist_mux_bucket_index(router, &signature);
    cwist_mux_route *curr = router->buckets[idx];
    
    // 1. Try exact match (O(1))
    while (curr) {
        if (curr->method == req->method &&
            curr->signature_hi == signature.hi &&
            curr->signature_lo == signature.lo &&
            curr->path && curr->path->data &&
            strcmp(curr->path->data, path) == 0) {
            if (curr->middleware) {
                run_middleware_chain(curr->middleware, req, res, curr->handler);
            } else {
                curr->handler(req, res);
            }
            return true;
        }
        curr = curr->bucket_next;
    }

    // 2. Try parametric matching (linear search)
    curr = router->param_routes;
    while (curr) {
        if (curr->method == req->method && curr->path && curr->path->data) {
            cwist_query_map *extracted_params = NULL;
            if (match_parametric_route(curr->path->data, path, &extracted_params)) {
                if (extracted_params) {
                    if (req->path_params) {
                        cwist_query_map_destroy(req->path_params);
                    }
                    req->path_params = extracted_params;
                }
                if (curr->middleware) {
                    run_middleware_chain(curr->middleware, req, res, curr->handler);
                } else {
                    curr->handler(req, res);
                }
                return true;
            }
        }
        curr = curr->param_next;
    }

    // 3. Try wildcard matching (linear search)
    curr = router->wildcard_routes;
    while (curr) {
        if (curr->method == req->method && curr->path && curr->path->data) {
            if (match_wildcard_route(curr->path->data, path)) {
                if (curr->middleware) {
                    run_middleware_chain(curr->middleware, req, res, curr->handler);
                } else {
                    curr->handler(req, res);
                }
                return true;
            }
        }
        curr = curr->param_next;
    }

    return false;
}
