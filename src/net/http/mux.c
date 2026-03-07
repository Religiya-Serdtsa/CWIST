#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/mux.h>
#include <cwist/net/http/http.h>
#include <cwist/core/mem/alloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define CWIST_MUX_DEFAULT_BUCKETS 4099

typedef struct {
    uint64_t hi;
    uint64_t lo;
} cwist_mux_signature;

/* Nam Byeong-gil (Gu-iljib) inspired orthogonal layout for segment mixing. */
static const uint8_t NAM_LS_PRIMARY[4][4] = {
    {0, 1, 2, 3},
    {1, 2, 3, 0},
    {2, 3, 0, 1},
    {3, 0, 1, 2}
};

static const uint8_t NAM_LS_SECONDARY[4][4] = {
    {0, 1, 2, 3},
    {3, 0, 1, 2},
    {1, 2, 3, 0},
    {2, 3, 0, 1}
};

static inline uint64_t mux_rotl64(uint64_t v, unsigned int r) {
    return (v << r) | (v >> (64U - r));
}

static uint8_t nam_latin_merge(uint8_t lhs, uint8_t rhs) {
    uint8_t row = lhs & 0x3;
    uint8_t col = rhs & 0x3;
    uint8_t a = NAM_LS_PRIMARY[row][col];
    uint8_t b = NAM_LS_SECONDARY[col][row];
    return (uint8_t)((a << 2) | b);
}

static void cwist_mux_mix_segment(cwist_mux_signature *sig, const char *segment, size_t len, size_t seg_idx) {
    uint64_t acc = 0xA0761D6478BD642FULL ^ ((uint64_t)len << (seg_idx & 15));
    for (size_t i = 0; i < len; ++i) {
        uint8_t ch = (uint8_t)segment[i];
        uint8_t latin = nam_latin_merge(ch, (uint8_t)(i + seg_idx));
        uint64_t delta = ((uint64_t)latin << 48) |
                         ((uint64_t)ch << 24) |
                         ((uint64_t)(len - i) << 8) |
                         (uint64_t)seg_idx;
        acc ^= mux_rotl64(delta, (unsigned int)(((i * 11) + seg_idx * 5) & 63));
    }
    sig->hi ^= mux_rotl64(acc ^ sig->lo, (unsigned int)(((seg_idx * 13) + 7) & 63));
    sig->lo += mux_rotl64(acc + sig->hi, (unsigned int)(((seg_idx * 17) + 3) & 63));
}

static cwist_mux_signature cwist_mux_signature_from_path(cwist_http_method_t method, const char *path) {
    cwist_mux_signature sig = {
        .hi = 0x6a09e667f3bcc909ULL ^ ((uint64_t)method * 0x9e3779b97f4a7c15ULL),
        .lo = 0xbb67ae8584caa73bULL ^ (((uint64_t)method << 40) | 0x100000001b3ULL)
    };

    if (!path) {
        cwist_mux_mix_segment(&sig, "", 0, 0);
    } else {
        size_t seg_idx = 0;
        const char *cursor = path;
        while (*cursor) {
            while (*cursor == '/') cursor++;
            const char *start = cursor;
            while (*cursor && *cursor != '/') cursor++;
            size_t len = (size_t)(cursor - start);
            cwist_mux_mix_segment(&sig, start, len, seg_idx++);
            if (!*cursor) break;
        }
        if (seg_idx == 0) {
            cwist_mux_mix_segment(&sig, "", 0, 0);
        }
    }
    sig.hi ^= mux_rotl64(sig.lo, 29);
    sig.lo ^= mux_rotl64(sig.hi, 19);
    return sig;
}

static size_t cwist_mux_bucket_index(const cwist_mux_router *router, const cwist_mux_signature *sig) {
    uint64_t mix = sig->hi ^ mux_rotl64(sig->lo, 23);
    return (size_t)(mix % router->bucket_count);
}

/* --- Mux Router Implementation --- */

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
    cwist_free(router->buckets);
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
    route->bucket_next = NULL;
    route->next = router->routes;
    cwist_mux_signature signature = cwist_mux_signature_from_path(method, path);
    route->signature_hi = signature.hi;
    route->signature_lo = signature.lo;

    size_t idx = cwist_mux_bucket_index(router, &signature);
    route->bucket_next = router->buckets[idx];
    router->buckets[idx] = route;
    router->routes = route;
}

bool cwist_mux_serve(cwist_mux_router *router, cwist_http_request *req, cwist_http_response *res) {
    if (!router || !req || !res) return false;

    const char *path = (req->path && req->path->data) ? req->path->data : "/";
    cwist_mux_signature signature = cwist_mux_signature_from_path(req->method, path);
    size_t idx = cwist_mux_bucket_index(router, &signature);
    cwist_mux_route *curr = router->buckets[idx];
    while (curr) {
        if (curr->method == req->method &&
            curr->signature_hi == signature.hi &&
            curr->signature_lo == signature.lo &&
            curr->path && curr->path->data &&
            strcmp(curr->path->data, path) == 0) {
            curr->handler(req, res);
            return true;
        }
        curr = curr->bucket_next;
    }
    return false;
}
