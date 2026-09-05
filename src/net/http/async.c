/**
 * @file async.c
 * @brief Deferred-response (async handler) implementation.
 *
 * See include/cwist/net/http/async.h for the model.  The dispatch path
 * (app_serve_parsed_request) detects res->deferred right after the handler
 * returns, acknowledges the handoff, and leaves req/res/fd/conn untouched;
 * from then on the cwist_async completion path owns them.
 *
 * Lifetime race: a foreign thread may complete (and thus want to free
 * req/res/the handle) before the dispatch thread has observed res->deferred.
 * Completion therefore spins on a->ack, which the dispatch path sets while
 * the objects are still guaranteed alive; only afterwards are they freed.
 * On the C1M path the completion always runs on the same reactor thread that
 * dispatched, so the spin never actually waits there.
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/async.h>
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/shutdown.h>
#include <cwist/sys/job/scheduler.h>
#include <cwist/core/mem/alloc.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <sched.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum cwist_async_state {
    CWIST_ASYNC_ST_PENDING = 0,
    CWIST_ASYNC_ST_CLAIMED
};

struct cwist_async {
    _Atomic int state;
    _Atomic bool ack;             /* Dispatch path observed the handoff. */
    cwist_http_request *req;
    cwist_http_response *res;     /* Handler's response (request arena). */
    cwist_http_response *final_res;
    bool final_res_owned;         /* final_res came from respond_with. */
    int client_fd;
    bool keep_alive;
    struct cwist_app *app;
    cwist_reactor_t *reactor;     /* NULL on the classic pool path. */
    cwist_http_async_conn_t *conn;
    cwist_reactor_post_t post;
};

static void cwist_async_reactor_complete(void *ctx);

static const char *cwist_async_reason(cwist_http_status_t status) {
    switch (status) {
        case CWIST_HTTP_OK:                  return "OK";
        case CWIST_HTTP_CREATED:             return "Created";
        case CWIST_HTTP_NO_CONTENT:          return "No Content";
        case CWIST_HTTP_PARTIAL_CONTENT:     return "Partial Content";
        case CWIST_HTTP_NOT_MODIFIED:        return "Not Modified";
        case CWIST_HTTP_BAD_REQUEST:         return "Bad Request";
        case CWIST_HTTP_UNAUTHORIZED:        return "Unauthorized";
        case CWIST_HTTP_FORBIDDEN:           return "Forbidden";
        case CWIST_HTTP_NOT_FOUND:           return "Not Found";
        case CWIST_HTTP_RANGE_NOT_SATISFIABLE: return "Range Not Satisfiable";
        case CWIST_HTTP_INTERNAL_ERROR:      return "Internal Server Error";
        case CWIST_HTTP_NOT_IMPLEMENTED:     return "Not Implemented";
        case CWIST_HTTP_SERVICE_UNAVAILABLE: return "Service Unavailable";
        case CWIST_HTTP_GATEWAY_TIMEOUT:     return "Gateway Timeout";
        default:                             return "Status";
    }
}

cwist_async *cwist_async_defer(cwist_http_request *req, cwist_http_response *res) {
    if (!req || !res || res->deferred) return NULL;
    cwist_async *a = cwist_alloc(sizeof(*a));
    if (!a) return NULL;
    memset(a, 0, sizeof(*a));
    atomic_init(&a->state, CWIST_ASYNC_ST_PENDING);
    atomic_init(&a->ack, false);
    a->req = req;
    a->res = res;
    a->final_res = res;
    a->client_fd = req->client_fd;
    a->keep_alive = req->keep_alive && res->keep_alive;
    a->app = req->app;
    cwist_http_async_conn_t *conn = (cwist_http_async_conn_t *)req->async_conn;
    if (conn) {
        a->reactor = conn->reactor;
        a->conn = conn;
    }
    a->post.cb = cwist_async_reactor_complete;
    a->post.ctx = a;
    res->async = a;
    res->deferred = true;
    return a;
}

void cwist_async_dispatch_ack(cwist_async *a) {
    if (a) atomic_store_explicit(&a->ack, true, memory_order_release);
}

static bool cwist_async_claim(cwist_async *a) {
    int expected = CWIST_ASYNC_ST_PENDING;
    return atomic_compare_exchange_strong_explicit(&a->state, &expected,
                                                   CWIST_ASYNC_ST_CLAIMED,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire);
}

static void cwist_async_reactor_complete(void *ctx);

/* Send the final response, re-arm or close the connection, then release the
 * request/response pair and the handle itself. */
static void cwist_async_complete(cwist_async *a) {
    cwist_http_response *res = a->final_res;
    bool keep = a->keep_alive && atomic_load(&g_cwist_running);
    res->keep_alive = keep;

    cwist_error_t err;
    if (a->reactor) {
        /* C1M: the fd is O_NONBLOCK; take a bounded blocking write instead of
         * a POLLOUT-resumable writer (v2). */
        int fl = fcntl(a->client_fd, F_GETFL, 0);
        struct timeval old_tv;
        socklen_t old_tv_len = sizeof(old_tv);
        bool have_tv = getsockopt(a->client_fd, SOL_SOCKET, SO_SNDTIMEO, &old_tv, &old_tv_len) == 0;
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(a->client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (fl >= 0) fcntl(a->client_fd, F_SETFL, fl & ~O_NONBLOCK);
        err = cwist_http_send_response(a->client_fd, res);
        if (fl >= 0) fcntl(a->client_fd, F_SETFL, fl);
        if (have_tv) setsockopt(a->client_fd, SOL_SOCKET, SO_SNDTIMEO, &old_tv, old_tv_len);
    } else {
        err = cwist_http_send_response(a->client_fd, res);
    }
    bool ok = err.error.err_i16 == 0;

    if (a->reactor) {
        if (keep && ok) {
            cwist_http_async_rearm(a->client_fd, a->reactor, a->conn);
        } else {
            cwist_http_async_close(a->client_fd, a->conn);
        }
    } else {
        if (keep && ok && a->app) {
            cwist_http_pool_rearm_current(a->client_fd, cwist_app_http_handler, a->app);
        } else {
            close(a->client_fd);
        }
    }

    /* See the file header: never free before dispatch observed the defer. */
    while (!atomic_load_explicit(&a->ack, memory_order_acquire)) sched_yield();

    if (a->final_res_owned) cwist_http_response_destroy(res);
    cwist_http_response_destroy(a->res);
    cwist_http_request_destroy(a->req);
    cwist_free(a);
}

static void cwist_async_reactor_complete(void *ctx) {
    cwist_async_complete((cwist_async *)ctx);
}

static void cwist_async_finish(cwist_async *a) {
    if (a->reactor) {
        if (cwist_reactor_post(a->reactor, &a->post)) return;
        /* Reactor already gone (shutdown): fall through to inline close-out. */
    }
    cwist_async_complete(a);
}

/* --- Timeout ------------------------------------------------------------- */

static pthread_once_t g_timeout_once = PTHREAD_ONCE_INIT;
static cwist_scheduler_t *g_timeout_sched;

static void cwist_async_timeout_sched_init(void) {
    g_timeout_sched = cwist_scheduler_create(1, 64);
}

static void cwist_async_timeout_job(void *arg) {
    cwist_async *a = (cwist_async *)arg;
    if (!cwist_async_claim(a)) return; /* A real response beat the timeout. */
    cwist_http_response *res = a->res;
    res->status_code = CWIST_HTTP_GATEWAY_TIMEOUT;
    cwist_sstring_assign(res->status_text, (char *)"Gateway Timeout");
    cwist_sstring_assign(res->body, (char *)"Gateway Timeout");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    cwist_async_finish(a);
}

void cwist_async_set_timeout(cwist_async *a, uint64_t ms) {
    if (!a || ms == 0) return;
    pthread_once(&g_timeout_once, cwist_async_timeout_sched_init);
    if (!g_timeout_sched) return;
    cwist_scheduler_schedule(g_timeout_sched, cwist_async_timeout_job, a, ms);
}

/* --- Completion API ------------------------------------------------------ */

bool cwist_async_respond(cwist_async *a, cwist_http_status_t status,
                         const char *content_type, const void *body, size_t len) {
    if (!a || !cwist_async_claim(a)) return false;
    cwist_http_response *res = a->res;
    res->status_code = status;
    cwist_sstring_assign(res->status_text, (char *)cwist_async_reason(status));
    if (body && len > 0) {
        cwist_sstring_assign_len(res->body, (const char *)body, len);
    }
    if (content_type) {
        cwist_http_header_add(&res->headers, "Content-Type", content_type);
    }
    cwist_async_finish(a);
    return true;
}

bool cwist_async_respond_with(cwist_async *a, cwist_http_response *res) {
    if (!a || !res || !cwist_async_claim(a)) return false;
    a->final_res = res;
    a->final_res_owned = true;
    cwist_async_finish(a);
    return true;
}

bool cwist_async_abort(cwist_async *a, cwist_http_status_t status) {
    if (!a || !cwist_async_claim(a)) return false;
    cwist_http_response *res = a->res;
    res->status_code = status;
    const char *reason = cwist_async_reason(status);
    cwist_sstring_assign(res->status_text, (char *)reason);
    cwist_sstring_assign(res->body, (char *)reason);
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    a->keep_alive = false;
    cwist_async_finish(a);
    return true;
}
