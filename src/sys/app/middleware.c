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

/* --- Request ID Middleware --- */

static pthread_mutex_t rid_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int rid_seed = 0;

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

cwist_middleware_func cwist_mw_request_id(const char *header_name) {
    CWIST_UNUSED(header_name);
    return cwist_mw_request_id_handler;
}

/* --- Access Log Middleware --- */

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void cwist_mw_access_log_handler(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    struct timeval start, end;
    gettimeofday(&start, NULL);

    next(req, res);

    gettimeofday(&end, NULL);
    long msec = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_usec - start.tv_usec) / 1000;

    // get request id from header.
    const char *rid = cwist_http_header_get(res->headers, "X-Request-Id");
    
    pthread_mutex_lock(&log_mutex);
    printf("[%s] %s %s -> %d (%ldms) [Req: %zu bytes, Res: %zu bytes]\n",
           rid ? rid : "-",
           cwist_http_method_to_string(req->method),
           req->path->data,
           res->status_code,
           msec,
           req->body ? req->body->size : 0,
           res->body ? res->body->size : 0);
    pthread_mutex_unlock(&log_mutex);
}

cwist_middleware_func cwist_mw_access_log(cwist_log_format_t format) {
    CWIST_UNUSED(format);
    return cwist_mw_access_log_handler;
}

/* --- Rate Limiter Middleware --- */

typedef struct {
    char ip[46];
    long last_reset;
    int count;
} ip_limit_t;

#define MAX_IP_TRACK 1024
static ip_limit_t ip_cache[MAX_IP_TRACK];
static int ip_cache_count = 0;
static pthread_mutex_t rate_mutex = PTHREAD_MUTEX_INITIALIZER;

void cwist_mw_rate_limit_ip_handler(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {

    // Get client ip from fd
    cwist_sstring *ip = cwist_get_client_ip_from_fd(req->client_fd);

    time_t now = time(NULL);
    ip_limit_t *found = NULL;

    pthread_mutex_lock(&rate_mutex);

    // if ip is found on ip_cache, set found = &ip_cache[i];
    for (int i = 0; i < ip_cache_count; i++) {
        if (!cwist_sstring_compare(ip, ip_cache[i].ip)) {
            found = &ip_cache[i];
            break;
        }
    }

    // if not found, add current ip to ip_cache
    if (!found && ip_cache_count < MAX_IP_TRACK) {
        found = &ip_cache[ip_cache_count++];
        strncpy(found->ip, ip->data, sizeof(found->ip) - 1);
        found->last_reset = now;
        found->count = 0;
    }

    // if found, refresh reset time
    if (found) {
        if (now - found->last_reset >= 60) {
            found->last_reset = now;
            found->count = 0;
        }

        // if cound count is more than 60, block request
        if (found->count >= 60) {
            pthread_mutex_unlock(&rate_mutex);
            res->status_code = 429;
            cwist_sstring_assign(res->body, "Too Many Requests");
            cwist_http_header_add(&res->headers, "Retry-After", "60");
            return;
        }
        found->count++;
    }
    cwist_sstring_destroy(ip);
    pthread_mutex_unlock(&rate_mutex);

    next(req, res);
}

cwist_middleware_func cwist_mw_rate_limit_ip(int requests_per_minute) {
    CWIST_UNUSED(requests_per_minute);
    return cwist_mw_rate_limit_ip_handler;
}

/* --- CORS Middleware --- */

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

cwist_middleware_func cwist_mw_cors(void) {
    return cwist_mw_cors_handler;
}
