#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/middleware.h>
#include <cwist/net/http/http.h>
#include <cwist/core/macros.h>
#include <cwist/core/utils/json_builder.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>

/**
 * @file middleware.c
 * @brief Built-in middleware implementations for request IDs, logging, rate limits, CORS, and JWT auth.
 */

static pthread_mutex_t rid_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int rid_seed = 0;

/**
 * @brief Generate a short pseudo-random request identifier for logging and tracing.
 * @return Heap-allocated 16-character identifier string.
 */
static char *generate_request_id() {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    char *id = cwist_alloc(17);
    
    pthread_mutex_lock(&rid_mutex);
    if (rid_seed == 0) rid_seed = (unsigned int)time(NULL) ^ (unsigned int)pthread_self();
    unsigned int seed = rid_seed++;
    pthread_mutex_unlock(&rid_mutex);

    for (int i = 0; i < 16; i++) {
        id[i] = charset[rand_r(&seed) % (sizeof(charset) - 1)];
    }
    id[16] = '\0';
    return id;
}

/**
 * @brief Attach an X-Request-Id header to both the request and the response.
 * @param req Incoming HTTP request.
 * @param res Outgoing HTTP response.
 * @param next Next middleware or final handler in the chain.
 */
void cwist_mw_request_id_handler(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    const char *header_name = "X-Request-Id";
    char *existing = cwist_http_header_get(req->headers, header_name);
    char *rid;

    if (existing) {
        rid = cwist_strdup(existing);
    } else {
        rid = generate_request_id();
        cwist_http_header_add(&req->headers, header_name, rid);
    }

    cwist_http_header_add(&res->headers, header_name, rid);

    next(req, res);
    cwist_free(rid);
}

/**
 * @brief Return the built-in request ID middleware.
 * @param header_name Currently unused custom header override.
 * @return Middleware function pointer for request ID injection.
 */
cwist_middleware_func cwist_mw_request_id(const char *header_name) {
    CWIST_UNUSED(header_name);
    return cwist_mw_request_id_handler;
}

/* --- Access Log Middleware --- */

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void format_clf_time(char *buf, size_t len, const struct timeval *tv) {
    struct tm tm;
    localtime_r(&tv->tv_sec, &tm);
    char tzbuf[8];
    strftime(tzbuf, sizeof(tzbuf), "%z", &tm);
    strftime(buf, len, "%d/%b/%Y:%H:%M:%S", &tm);
    size_t pos = strlen(buf);
    snprintf(buf + pos, len - pos, " %s", tzbuf);
}

static void format_iso8601_time(char *buf, size_t len, const struct timeval *tv) {
    struct tm tm;
    gmtime_r(&tv->tv_sec, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%S", &tm);
    size_t pos = strlen(buf);
    snprintf(buf + pos, len - pos, ".%03ldZ", tv->tv_usec / 1000);
}

static void cwist_mw_access_log_common_handler(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    struct timeval start, end;
    gettimeofday(&start, NULL);
    next(req, res);
    gettimeofday(&end, NULL);

    const char *rid = cwist_http_header_get(res->headers, "X-Request-Id");
    cwist_sstring *ip = cwist_get_client_ip_from_fd(req->client_fd);
    const char *ip_str = ip ? ip->data : "-";

    char time_buf[64];
    format_clf_time(time_buf, sizeof(time_buf), &end);

    size_t res_bytes = res->body ? res->body->size : 0;

    pthread_mutex_lock(&log_mutex);
    printf("%s - %s [%s] \"%s %s %s\" %d %zu\n",
           ip_str, rid ? rid : "-", time_buf,
           cwist_http_method_to_string(req->method),
           req->path->data,
           req->version ? req->version->data : "HTTP/1.1",
           res->status_code, res_bytes);
    pthread_mutex_unlock(&log_mutex);

    if (ip) cwist_sstring_destroy(ip);
}

static void cwist_mw_access_log_combined_handler(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    struct timeval start, end;
    gettimeofday(&start, NULL);
    next(req, res);
    gettimeofday(&end, NULL);

    const char *rid = cwist_http_header_get(res->headers, "X-Request-Id");
    cwist_sstring *ip = cwist_get_client_ip_from_fd(req->client_fd);
    const char *ip_str = ip ? ip->data : "-";

    char time_buf[64];
    format_clf_time(time_buf, sizeof(time_buf), &end);

    const char *referer = cwist_http_header_get(req->headers, "Referer");
    const char *user_agent = cwist_http_header_get(req->headers, "User-Agent");
    size_t res_bytes = res->body ? res->body->size : 0;

    pthread_mutex_lock(&log_mutex);
    printf("%s - %s [%s] \"%s %s %s\" %d %zu \"%s\" \"%s\"\n",
           ip_str, rid ? rid : "-", time_buf,
           cwist_http_method_to_string(req->method),
           req->path->data,
           req->version ? req->version->data : "HTTP/1.1",
           res->status_code, res_bytes,
           referer ? referer : "-",
           user_agent ? user_agent : "-");
    pthread_mutex_unlock(&log_mutex);

    if (ip) cwist_sstring_destroy(ip);
}

static void cwist_mw_access_log_json_handler(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    struct timeval start, end;
    gettimeofday(&start, NULL);
    next(req, res);
    gettimeofday(&end, NULL);
    long msec = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_usec - start.tv_usec) / 1000;

    const char *rid = cwist_http_header_get(res->headers, "X-Request-Id");
    cwist_sstring *ip = cwist_get_client_ip_from_fd(req->client_fd);
    const char *ip_str = ip ? ip->data : "-";

    char time_buf[64];
    format_iso8601_time(time_buf, sizeof(time_buf), &end);

    size_t req_bytes = req->body ? req->body->size : 0;
    size_t res_bytes = res->body ? res->body->size : 0;

    pthread_mutex_lock(&log_mutex);
    printf("{\"time\":\"%s\",\"client\":\"%s\",\"rid\":\"%s\",\"method\":\"%s\",\"path\":\"%s\",\"protocol\":\"%s\",\"status\":%d,\"duration_ms\":%ld,\"req_bytes\":%zu,\"res_bytes\":%zu}\n",
           time_buf, ip_str, rid ? rid : "-",
           cwist_http_method_to_string(req->method),
           req->path->data,
           req->version ? req->version->data : "HTTP/1.1",
           res->status_code, msec, req_bytes, res_bytes);
    pthread_mutex_unlock(&log_mutex);

    if (ip) cwist_sstring_destroy(ip);
}

/**
 * @brief Return the built-in access-log middleware.
 * @param format Log format selector (COMMON, COMBINED, JSON).
 * @return Middleware function pointer for access logging.
 */
cwist_middleware_func cwist_mw_access_log(cwist_log_format_t format) {
    switch (format) {
        case CWIST_LOG_COMMON: return cwist_mw_access_log_common_handler;
        case CWIST_LOG_COMBINED: return cwist_mw_access_log_combined_handler;
        case CWIST_LOG_JSON: return cwist_mw_access_log_json_handler;
        default: return cwist_mw_access_log_common_handler;
    }
}

/* --- Metrics Middleware --- */

#include <cwist/sys/metrics/metrics.h>

/**
 * @brief Increment request counter and observe duration for Prometheus metrics.
 * @param req Incoming HTTP request.
 * @param res Outgoing HTTP response.
 * @param next Next middleware or final handler in the chain.
 */
void cwist_mw_metrics_handler(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    next(req, res);
    clock_gettime(CLOCK_MONOTONIC, &end);
    long duration_ns = (end.tv_sec - start.tv_sec) * 1000000000L + (end.tv_nsec - start.tv_nsec);
    cwist_metrics_registry_t *reg = cwist_metrics_registry();
    if (reg) {
        cwist_metric_inc(reg, CWIST_METRIC_REQUESTS_TOTAL);
        cwist_metric_add(reg, CWIST_METRIC_REQUEST_DURATION_NS, (uintmax_t)duration_ns);
    }
}

/**
 * @brief Return the built-in metrics collection middleware.
 * @return Middleware function pointer for request metrics.
 */
cwist_middleware_func cwist_mw_metrics(void) {
    return cwist_mw_metrics_handler;
}

/* --- Rate Limiter Middleware --- */

#include <ttak/limit/limit.h>

typedef struct {
    char ip[46];
    ttak_token_bucket_t bucket;
    bool active;
} ip_bucket_t;

#define MAX_IP_BUCKETS 1024
static ip_bucket_t ip_buckets[MAX_IP_BUCKETS];
static int ip_bucket_count = 0;
static pthread_mutex_t rate_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int rpm;
} rate_limit_ctx_t;

static void cwist_mw_rate_limit_ip_handler(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    rate_limit_ctx_t *ctx = (rate_limit_ctx_t *)req->private_data;
    int rpm = (ctx && ctx->rpm > 0) ? ctx->rpm : 60;

    cwist_sstring *ip = cwist_get_client_ip_from_fd(req->client_fd);
    if (!ip) {
        next(req, res);
        return;
    }

    pthread_mutex_lock(&rate_mutex);
    ip_bucket_t *found = NULL;
    for (int i = 0; i < ip_bucket_count; i++) {
        if (ip_buckets[i].active && strcmp(ip_buckets[i].ip, ip->data) == 0) {
            found = &ip_buckets[i];
            break;
        }
    }

    if (!found && ip_bucket_count < MAX_IP_BUCKETS) {
        found = &ip_buckets[ip_bucket_count++];
        strncpy(found->ip, ip->data, sizeof(found->ip) - 1);
        found->ip[sizeof(found->ip) - 1] = '\0';
        found->active = true;
        double rate = rpm / 60.0;
        ttak_token_bucket_init(&found->bucket, rate, (double)rpm);
    }

    bool allowed = false;
    if (found) {
        allowed = ttak_token_bucket_consume(&found->bucket, 1.0);
    }
    cwist_sstring_destroy(ip);
    pthread_mutex_unlock(&rate_mutex);

    if (!allowed) {
        res->status_code = 429;
        cwist_sstring_assign(res->body, "Too Many Requests");
        char retry_buf[16];
        snprintf(retry_buf, sizeof(retry_buf), "%d", 60 / rpm > 0 ? 60 / rpm : 1);
        cwist_http_header_add(&res->headers, "Retry-After", retry_buf);
        return;
    }

    next(req, res);
}

#define CWIST_RATE_LIMIT_MAX_CFGS 8

typedef struct {
    int rpm;
} rate_limit_cfg_t;

static rate_limit_cfg_t s_rate_cfgs[CWIST_RATE_LIMIT_MAX_CFGS];
static int s_rate_cfg_count = 0;
static pthread_mutex_t s_rate_cfg_mutex = PTHREAD_MUTEX_INITIALIZER;

#define CWIST_RATE_LIMIT_DEFINE_WRAPPER(N) \
static void cwist_mw_rate_limit_wrap_##N(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) { \
    rate_limit_ctx_t ctx = { .rpm = s_rate_cfgs[N].rpm }; \
    void *prev = req->private_data; \
    req->private_data = &ctx; \
    cwist_mw_rate_limit_ip_handler(req, res, next); \
    req->private_data = prev; \
}

CWIST_RATE_LIMIT_DEFINE_WRAPPER(0)
CWIST_RATE_LIMIT_DEFINE_WRAPPER(1)
CWIST_RATE_LIMIT_DEFINE_WRAPPER(2)
CWIST_RATE_LIMIT_DEFINE_WRAPPER(3)
CWIST_RATE_LIMIT_DEFINE_WRAPPER(4)
CWIST_RATE_LIMIT_DEFINE_WRAPPER(5)
CWIST_RATE_LIMIT_DEFINE_WRAPPER(6)
CWIST_RATE_LIMIT_DEFINE_WRAPPER(7)

static cwist_middleware_func s_rate_wrappers[CWIST_RATE_LIMIT_MAX_CFGS] = {
    cwist_mw_rate_limit_wrap_0, cwist_mw_rate_limit_wrap_1, cwist_mw_rate_limit_wrap_2, cwist_mw_rate_limit_wrap_3,
    cwist_mw_rate_limit_wrap_4, cwist_mw_rate_limit_wrap_5, cwist_mw_rate_limit_wrap_6, cwist_mw_rate_limit_wrap_7,
};

/**
 * @brief Return the built-in per-IP rate-limiter middleware.
 * @param requests_per_minute Allowed requests per minute (token bucket rate).
 * @return Middleware function pointer for per-IP rate limiting.
 */
void cwist_mw_rate_limit_reset(void) {
    pthread_mutex_lock(&rate_mutex);
    ip_bucket_count = 0;
    pthread_mutex_unlock(&rate_mutex);
}

cwist_middleware_func cwist_mw_rate_limit_ip(int requests_per_minute) {
    if (requests_per_minute <= 0) requests_per_minute = 60;

    pthread_mutex_lock(&s_rate_cfg_mutex);

    for (int i = 0; i < s_rate_cfg_count; i++) {
        if (s_rate_cfgs[i].rpm == requests_per_minute) {
            pthread_mutex_unlock(&s_rate_cfg_mutex);
            return s_rate_wrappers[i];
        }
    }

    if (s_rate_cfg_count >= CWIST_RATE_LIMIT_MAX_CFGS) {
        pthread_mutex_unlock(&s_rate_cfg_mutex);
        fprintf(stderr, "[CWIST] cwist_mw_rate_limit_ip: maximum config slots exceeded\n");
        return s_rate_wrappers[0];
    }

    int slot = s_rate_cfg_count++;
    s_rate_cfgs[slot].rpm = requests_per_minute;

    pthread_mutex_unlock(&s_rate_cfg_mutex);
    return s_rate_wrappers[slot];
}

/* --- CORS Middleware --- */

/**
 * @brief Inject permissive CORS headers and short-circuit preflight requests.
 * @param req Incoming HTTP request.
 * @param res Outgoing HTTP response.
 * @param next Next middleware or final handler in the chain.
 */
void cwist_mw_cors_handler(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    // Add standard CORS headers
    cwist_http_header_add(&res->headers, "Access-Control-Allow-Origin", "*");

    // Handle Preflight (OPTIONS)
    if (req->method == CWIST_HTTP_OPTIONS) {
        cwist_http_header_add(&res->headers, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, PATCH, OPTIONS, HEAD");
        cwist_http_header_add(&res->headers, "Access-Control-Allow-Headers", "Content-Type, Authorization, X-Request-Id");
        cwist_http_header_add(&res->headers, "Access-Control-Max-Age", "86400"); // 24 hours

        res->status_code = CWIST_HTTP_NO_CONTENT; // 204 No Content
        // Short-circuit: do not call next()
        return;
    }

    next(req, res);
}

/**
 * @brief Return the built-in permissive CORS middleware.
 * @return Middleware function pointer for CORS handling.
 */
cwist_middleware_func cwist_mw_cors(void) {
    return cwist_mw_cors_handler;
}

/* --- JWT Authentication Middleware --- */

/*
 * We use req->private_data to carry the decoded claims across the middleware
 * boundary into the downstream handler.  The previous value of private_data
 * is saved/restored so other middleware can also use that field without
 * conflict.
 *
 * The context is stack-allocated inside each wrapper function.  It is only
 * valid during the synchronous execution of the middleware chain; no pointer
 * to the context escapes to asynchronous code.
 */

#define CWIST_JWT_CTX_MAGIC 0x4A574354UL  /* "JWCT" */

typedef struct {
    unsigned long magic;             ///< Must equal CWIST_JWT_CTX_MAGIC.
    const char *secret;              ///< Signing secret (borrowed, never freed here).
    cwist_jwt_claims *claims;        ///< Decoded claims; owned by this struct.
    void *prev_private_data;         ///< Previous req->private_data value.
} cwist_jwt_ctx_t;

/**
 * @brief Validate a bearer token and expose its decoded claims to downstream handlers.
 * @param req Incoming HTTP request.
 * @param res Outgoing HTTP response.
 * @param next Next middleware or final handler in the chain.
 */
void cwist_mw_jwt_auth_handler(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    /* Retrieve the secret stored in the context tag */
    cwist_jwt_ctx_t *ctx = (cwist_jwt_ctx_t *)req->private_data;
    if (!ctx) {
        /* Should not happen if the middleware was set up correctly */
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "{\"error\":\"JWT middleware misconfigured\"}");
        cwist_http_header_add(&res->headers, "Content-Type", "application/json");
        return;
    }

    const char *secret = ctx->secret;

    /* Extract Bearer token from Authorization header */
    char *auth_header = cwist_http_header_get(req->headers, "Authorization");
    if (!auth_header) {
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->body, "{\"error\":\"Missing Authorization header\"}");
        cwist_http_header_add(&res->headers, "Content-Type", "application/json");
        return;
    }

    /* Expect "Bearer <token>" */
    if (strncmp(auth_header, "Bearer ", 7) != 0) {
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->body, "{\"error\":\"Invalid Authorization scheme\"}");
        cwist_http_header_add(&res->headers, "Content-Type", "application/json");
        return;
    }

    const char *token = auth_header + 7;

    cwist_jwt_claims *claims = cwist_jwt_verify(token, secret);
    if (!claims) {
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->body, "{\"error\":\"Invalid or expired token\"}");
        cwist_http_header_add(&res->headers, "Content-Type", "application/json");
        return;
    }

    /* Stash claims so downstream handlers can retrieve them */
    ctx->claims = claims;

    next(req, res);

    /* Clean up after the chain returns */
    cwist_jwt_claims_destroy(claims);
    ctx->claims = NULL;
}

/*
 * Factory — we use a small heap-allocated context to bind the secret to the
 * handler.  Because cwist_middleware_func is a plain function pointer we cannot
 * capture the secret in a closure, so we embed it in the req->private_data
 * field before dispatching.
 *
 * The returned function pointer is cwist_mw_jwt_auth_handler.  The secret is
 * stored in a per-request cwist_jwt_ctx_t that is pushed/popped around the
 * call so it does not trample any existing private_data.
 */

typedef struct {
    const char *secret;
} cwist_jwt_mw_cfg_t;

/* We keep a small static table of registered secrets.  A server rarely needs
 * more than a handful of distinct signing keys.  8 slots cover the common case
 * (e.g. one per audience / service) without dynamic allocation.  If more are
 * needed, increase CWIST_JWT_MAX_SECRETS and add the corresponding wrapper. */
#define CWIST_JWT_MAX_SECRETS 8

static cwist_jwt_mw_cfg_t s_jwt_cfgs[CWIST_JWT_MAX_SECRETS];
static int s_jwt_cfg_count = 0;
static pthread_mutex_t s_jwt_cfg_mutex = PTHREAD_MUTEX_INITIALIZER;

/* One wrapper function per registered secret slot.
 * The ctx is stack-allocated; its lifetime is confined to this function frame. */
#define CWIST_JWT_DEFINE_WRAPPER(N) \
static void cwist_mw_jwt_wrap_##N(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) { \
    cwist_jwt_ctx_t ctx = { \
        .magic = CWIST_JWT_CTX_MAGIC, \
        .secret = s_jwt_cfgs[N].secret, \
        .claims = NULL, \
        .prev_private_data = req->private_data \
    }; \
    req->private_data = &ctx; \
    cwist_mw_jwt_auth_handler(req, res, next); \
    req->private_data = ctx.prev_private_data; \
}

CWIST_JWT_DEFINE_WRAPPER(0)
CWIST_JWT_DEFINE_WRAPPER(1)
CWIST_JWT_DEFINE_WRAPPER(2)
CWIST_JWT_DEFINE_WRAPPER(3)
CWIST_JWT_DEFINE_WRAPPER(4)
CWIST_JWT_DEFINE_WRAPPER(5)
CWIST_JWT_DEFINE_WRAPPER(6)
CWIST_JWT_DEFINE_WRAPPER(7)

static cwist_middleware_func s_jwt_wrappers[CWIST_JWT_MAX_SECRETS] = {
    cwist_mw_jwt_wrap_0, cwist_mw_jwt_wrap_1, cwist_mw_jwt_wrap_2, cwist_mw_jwt_wrap_3,
    cwist_mw_jwt_wrap_4, cwist_mw_jwt_wrap_5, cwist_mw_jwt_wrap_6, cwist_mw_jwt_wrap_7,
};

/**
 * @brief Register a JWT secret in a static slot and return the matching middleware wrapper.
 * @param secret Borrowed signing secret used to verify bearer tokens.
 * @return Middleware wrapper bound to the supplied secret, or NULL on overflow.
 */
cwist_middleware_func cwist_mw_jwt_auth(const char *secret) {
    if (!secret) return NULL;

    pthread_mutex_lock(&s_jwt_cfg_mutex);

    /* Reuse existing slot for the same secret pointer */
    for (int i = 0; i < s_jwt_cfg_count; i++) {
        if (s_jwt_cfgs[i].secret == secret) {
            pthread_mutex_unlock(&s_jwt_cfg_mutex);
            return s_jwt_wrappers[i];
        }
    }

    if (s_jwt_cfg_count >= CWIST_JWT_MAX_SECRETS) {
        pthread_mutex_unlock(&s_jwt_cfg_mutex);
        fprintf(stderr, "[CWIST] cwist_mw_jwt_auth: maximum secret slots exceeded\n");
        return NULL;
    }

    int slot = s_jwt_cfg_count++;
    s_jwt_cfgs[slot].secret = secret;

    pthread_mutex_unlock(&s_jwt_cfg_mutex);
    return s_jwt_wrappers[slot];
}

/**
 * @brief Retrieve the active JWT claims object from request private_data when present.
 * @param req Request currently executing inside the JWT middleware chain.
 * @return Active decoded claims, or NULL when the request is not inside JWT auth.
 */
const cwist_jwt_claims *cwist_mw_jwt_get_claims(const cwist_http_request *req) {
    if (!req || !req->private_data) return NULL;
    cwist_jwt_ctx_t *ctx = (cwist_jwt_ctx_t *)req->private_data;
    /* Validate that private_data is actually a JWT context */
    if (ctx->magic != CWIST_JWT_CTX_MAGIC) return NULL;
    return ctx->claims;
}
