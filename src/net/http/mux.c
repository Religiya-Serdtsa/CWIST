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

/* Lo Shu magic square for the method-path grid, lifted from classical Joseon Sanhak texts. */
static const uint8_t CWIST_LO_SHU[3][3] = {
    {8, 1, 6},
    {3, 5, 7},
    {4, 9, 2}
};

/* Jeungseung Gaebangbeop coefficient matrix used by the Dawonsul solver. */
static const double JOSEON_RATIO_MATRIX[3][3] = {
    {1.0, 1.0, 1.0},
    {2.0, 3.0, 5.0},
    {5.0, 3.0, 2.0}
};

/**
 * @brief Rotate a 64-bit integer left by the requested number of bits.
 * @param v Input value.
 * @param r Rotation width in the inclusive range [0, 63].
 * @return Rotated value used by the signature mixer.
 */
static inline uint64_t mux_rotl64(uint64_t v, unsigned int r) {
    return (v << r) | (v >> (64U - r));
}

/**
 * @brief Fold two 2-bit symbols into a compact 4-bit latin-square code.
 * @param lhs First symbol, usually the route byte.
 * @param rhs Second symbol, usually the segment-relative position.
 * @return Merged nibble used to perturb the route signature.
 */
static uint8_t nam_latin_merge(uint8_t lhs, uint8_t rhs) {
    uint8_t row = lhs & 0x3;
    uint8_t col = rhs & 0x3;
    uint8_t a = NAM_LS_PRIMARY[row][col];
    uint8_t b = NAM_LS_SECONDARY[col][row];
    return (uint8_t)((a << 2) | b);
}

/**
 * @brief Mix one slash-delimited path segment into the route signature.
 * @param sig Accumulator updated in place.
 * @param segment Raw segment bytes without the separating slash.
 * @param len Number of bytes in @p segment.
 * @param seg_idx Zero-based segment index within the path.
 */
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

/* --- Joseon Sanhak-Inspired Matrix Helpers --- */

/**
 * @brief Solve a 3x3 linear system using Dawonsul-style elimination.
 * @param matrix Coefficient matrix (row-major order).
 * @param rhs Right-hand side vector.
 * @param out Solution vector written when the system is non-singular.
 * @return true when a stable solution was found.
 */
static bool cwist_dawonsul_solve3(const double matrix[3][3], const double rhs[3], double out[3]) {
    double aug[3][4];
    for (size_t r = 0; r < 3; ++r) {
        for (size_t c = 0; c < 3; ++c) {
            aug[r][c] = matrix[r][c];
        }
        aug[r][3] = rhs[r];
    }

    for (size_t pivot = 0; pivot < 3; ++pivot) {
        size_t best = pivot;
        double best_abs = fabs(aug[pivot][pivot]);
        for (size_t r = pivot + 1; r < 3; ++r) {
            double val = fabs(aug[r][pivot]);
            if (val > best_abs) {
                best_abs = val;
                best = r;
            }
        }
        if (best_abs < 1e-9) {
            return false;
        }
        if (best != pivot) {
            for (size_t c = pivot; c < 4; ++c) {
                double tmp = aug[pivot][c];
                aug[pivot][c] = aug[best][c];
                aug[best][c] = tmp;
            }
        }
        double inv = 1.0 / aug[pivot][pivot];
        for (size_t c = pivot; c < 4; ++c) {
            aug[pivot][c] *= inv;
        }
        for (size_t r = 0; r < 3; ++r) {
            if (r == pivot) continue;
            double factor = aug[r][pivot];
            for (size_t c = pivot; c < 4; ++c) {
                aug[r][c] -= factor * aug[pivot][c];
            }
        }
    }

    for (size_t i = 0; i < 3; ++i) {
        out[i] = aug[i][3];
    }
    return true;
}

/**
 * @brief Extract Jeungseung Gaebangbeop coefficients from the signature.
 * @param sig Route signature.
 * @param coeffs Output vector containing the solved weights.
 */
static void cwist_jungseung_coeffs(const cwist_mux_signature *sig, double coeffs[3]) {
    double rhs[3] = {
        1.0 + (double)(sig->hi & 0xFFFFULL),
        1.0 + (double)((sig->lo >> 16ULL) & 0xFFFFULL),
        1.0 + (double)((((sig->hi >> 32ULL) ^ sig->lo) & 0xFFFFULL))
    };

    if (!cwist_dawonsul_solve3(JOSEON_RATIO_MATRIX, rhs, coeffs)) {
        coeffs[0] = rhs[0];
        coeffs[1] = rhs[1];
        coeffs[2] = rhs[2];
    }
}

/**
 * @brief Select a Lo Shu magic-square coordinate from the signature.
 */
static uint8_t cwist_magic_square_coord(const cwist_mux_signature *sig) {
    size_t row = (size_t)((sig->hi ^ sig->lo) % 3ULL);
    size_t col = (size_t)(((mux_rotl64(sig->hi, 17) ^ (sig->lo >> 7)) % 3ULL));
    return CWIST_LO_SHU[row][col];
}

/**
 * @brief Apply an al-Kashi style iterative refinement to the magnitude guess.
 */
static double cwist_al_kashi_refine(double guess, double target) {
    double g = fabs(guess) + 1.0;
    double t = fabs(target) + 1.0;
    for (int i = 0; i < 2; ++i) {
        g = 0.5 * (g + t / g);
    }
    return g;
}

/**
 * @brief Predict the next state using a single Euler step.
 */
static double cwist_euler_predict(double state, double slope) {
    const double h = 0.125; /* Small integration step. */
    return state + slope * h;
}

/**
 * @brief Derive the lookup signature for an HTTP method and path pair.
 * @param method HTTP verb associated with the route.
 * @param path Route path to normalise and hash. NULL is treated as the root.
 * @return Two-lane signature suitable for bucket selection and fast equality checks.
 */
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

/**
 * @brief Select the bucket that should hold a specific route signature.
 * @param router Router whose bucket array is being indexed.
 * @param sig Signature produced for the route or request path.
 * @return Stable bucket index in the router's fixed bucket array.
 */
static size_t cwist_mux_bucket_index(const cwist_mux_router *router, const cwist_mux_signature *sig) {
    uint8_t magic = cwist_magic_square_coord(sig);
    double coeffs[3];
    cwist_jungseung_coeffs(sig, coeffs);

    double ratio_mix = coeffs[0] * 7.0 + coeffs[1] * 5.0 + coeffs[2] * 3.0;
    double refined = cwist_al_kashi_refine(ratio_mix, (double)(sig->hi | 1ULL));
    double slope = ((double)((sig->hi >> 8ULL) & 0xFFULL) - (double)((sig->lo >> 8ULL) & 0xFFULL)) / 64.0;
    double predicted = cwist_euler_predict(refined, slope);
    if (predicted < 0.0) {
        predicted = -predicted + (double)magic;
    }

    uint64_t mix = ((uint64_t)predicted) ^ mux_rotl64(sig->hi + sig->lo, magic % 61U);
    mix ^= mux_rotl64(((uint64_t)magic * 0x9e3779b185ebca87ULL), magic % 31U);
    return (size_t)(mix % router->bucket_count);
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
    route->next = router->routes;
    cwist_mux_signature signature = cwist_mux_signature_from_path(method, path);
    route->signature_hi = signature.hi;
    route->signature_lo = signature.lo;

    size_t idx = cwist_mux_bucket_index(router, &signature);
    route->bucket_next = router->buckets[idx];
    router->buckets[idx] = route;
    router->routes = route;
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
