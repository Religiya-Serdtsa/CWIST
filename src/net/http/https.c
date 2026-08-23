#define _POSIX_C_SOURCE 200809L

#include <cwist/net/http/https.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/sys/err/cwist_err.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/sys/app/shutdown.h>
#include "tls_chain.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <time.h>

/* Forward declaration: PQC layer applied inside TLS bootstrap */
bool cwist_tls_apply_pqc_layer(cwist_app *app, SSL_CTX *ctx);

/* Monotonic clock in milliseconds, for connection deadlines. */
static uint64_t cwist_https_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/**
 * @brief Poll the socket for the direction OpenSSL is waiting on.
 * @return 0 if the requested event is ready, -1 on timeout/error.
 */
static int cwist_ssl_wait(int fd, int ssl_error, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = 0 };
    if (ssl_error == SSL_ERROR_WANT_READ) {
        pfd.events = POLLIN;
    } else if (ssl_error == SSL_ERROR_WANT_WRITE) {
        pfd.events = POLLOUT;
    } else {
        return -1;
    }

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return -1;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    return 0;
}

struct https_thread_payload {
    int client_fd;
    cwist_https_context *ctx;
    void (*handler)(cwist_https_connection *, void *);
    void *user_ctx;
    SSL *pres_ssl;  /* non-NULL when the shepherd already finished the handshake */
};

/* --- Thread Pool for HTTPS --- */
#define HTTPS_TASK_QUEUE_SIZE 2097152

typedef struct {
    int client_fd;
    cwist_https_context *ctx;
    void (*handler)(cwist_https_connection *, void *);
    void *user_ctx;
    SSL *pres_ssl;
} https_pool_task_t;

typedef struct {
    pthread_t threads[2048];
    size_t    threads_size;
    https_pool_task_t queue[HTTPS_TASK_QUEUE_SIZE];
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t cond_not_empty;
    pthread_cond_t cond_not_full;
    int shutdown;
} https_thread_pool_t;

static https_thread_pool_t g_https_pool;
static bool g_https_pool_initialized = false;

// Forward declaration of existing https_thread_handler
static void *https_thread_handler(void *arg);
void https_pool_submit_ready(int client_fd, SSL *pres_ssl, cwist_https_context *ctx, void (*handler)(cwist_https_connection *, void *), void *user_ctx);
static cwist_error_t https_wrap_established(cwist_https_context *ctx, int client_fd, SSL *ssl, cwist_https_connection **conn);
int https_hs_shepherd_start(void);
void https_hs_shepherd_stop(void);

static void *https_pool_worker(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&g_https_pool.mutex);
        while (g_https_pool.count == 0 && !g_https_pool.shutdown) {
            pthread_cond_wait(&g_https_pool.cond_not_empty, &g_https_pool.mutex);
        }
        if (g_https_pool.shutdown) {
            pthread_mutex_unlock(&g_https_pool.mutex);
            break;
        }
        https_pool_task_t task = g_https_pool.queue[g_https_pool.head];
        g_https_pool.head = (g_https_pool.head + 1) % HTTPS_TASK_QUEUE_SIZE;
        g_https_pool.count--;
        pthread_cond_signal(&g_https_pool.cond_not_full);
        pthread_mutex_unlock(&g_https_pool.mutex);

        struct https_thread_payload *payload = cwist_alloc(sizeof(*payload));
        if (payload) {
            payload->client_fd = task.client_fd;
            payload->ctx = task.ctx;
            payload->handler = task.handler;
            payload->user_ctx = task.user_ctx;
            payload->pres_ssl = task.pres_ssl;
            https_thread_handler(payload);
        } else {
            if (task.pres_ssl) SSL_free(task.pres_ssl);
            close(task.client_fd);
        }
    }
    return NULL;
}

int https_pool_init(void) {
    if (g_https_pool_initialized) return 0;
    memset(&g_https_pool, 0, sizeof(g_https_pool));
    pthread_mutex_init(&g_https_pool.mutex, NULL);
    pthread_cond_init(&g_https_pool.cond_not_empty, NULL);
    pthread_cond_init(&g_https_pool.cond_not_full, NULL);
    for (int i = 0; i < get_optimal_thread_count(); i++) {
        if (pthread_create(&g_https_pool.threads[i], NULL, https_pool_worker, NULL) != 0) {
            return -1;
        }
    }
    g_https_pool_initialized = true;
    return 0;
}

void https_pool_submit(int client_fd, cwist_https_context *ctx, void (*handler)(cwist_https_connection *, void *), void *user_ctx) {
    https_pool_submit_ready(client_fd, NULL, ctx, handler, user_ctx);
}

/* Submit a connection whose TLS handshake already completed (pres_ssl). */
void https_pool_submit_ready(int client_fd, SSL *pres_ssl, cwist_https_context *ctx, void (*handler)(cwist_https_connection *, void *), void *user_ctx) {
    pthread_mutex_lock(&g_https_pool.mutex);
    while (g_https_pool.count >= HTTPS_TASK_QUEUE_SIZE && !g_https_pool.shutdown) {
        pthread_cond_wait(&g_https_pool.cond_not_full, &g_https_pool.mutex);
    }
    if (g_https_pool.shutdown) {
        pthread_mutex_unlock(&g_https_pool.mutex);
        if (pres_ssl) SSL_free(pres_ssl);
        close(client_fd);
        return;
    }
    g_https_pool.queue[g_https_pool.tail].client_fd = client_fd;
    g_https_pool.queue[g_https_pool.tail].ctx = ctx;
    g_https_pool.queue[g_https_pool.tail].handler = handler;
    g_https_pool.queue[g_https_pool.tail].user_ctx = user_ctx;
    g_https_pool.queue[g_https_pool.tail].pres_ssl = pres_ssl;
    g_https_pool.tail = (g_https_pool.tail + 1) % HTTPS_TASK_QUEUE_SIZE;
    g_https_pool.count++;
    pthread_cond_signal(&g_https_pool.cond_not_empty);
    pthread_mutex_unlock(&g_https_pool.mutex);
}

void https_pool_destroy(void) {
    if (!g_https_pool_initialized) return;
    https_hs_shepherd_stop();
    pthread_mutex_lock(&g_https_pool.mutex);
    g_https_pool.shutdown = 1;
    pthread_cond_broadcast(&g_https_pool.cond_not_empty);
    pthread_mutex_unlock(&g_https_pool.mutex);
    for (int i = 0; i < get_optimal_thread_count(); i++) {
        pthread_join(g_https_pool.threads[i], NULL);
    }
    pthread_mutex_destroy(&g_https_pool.mutex);
    pthread_cond_destroy(&g_https_pool.cond_not_empty);
    pthread_cond_destroy(&g_https_pool.cond_not_full);

    g_https_pool_initialized = false;
}
/* --- End Thread Pool --- */

/* --- TLS handshake shepherd ------------------------------------------------
 * Why this exists: the pool parks one worker per connection for the whole
 * connection lifetime.  A synchronous in-worker handshake made that fatal
 * under churn — a client that delays its ClientHello by a few seconds
 * (routine for h2load at thousands of connections per process) parked a
 * worker for the full 30 s poll, the pool collapsed to ~tens of
 * handshakes/s, the accept loop fell behind, the kernel accept queue
 * overflowed, and overflowing handshakes were dropped silently, leaving
 * clients ESTABLISHED with no server-side socket ("phantom" connections).
 *
 * The shepherd owns every in-progress handshake: cwist_https_dispatch()
 * tries SSL_accept once on a non-blocking socket; incomplete handshakes are
 * parked in a private epoll set and retried by a single shepherd thread,
 * which also reaps connections that exceed CWIST_HTTPS_HANDSHAKE_TIMEOUT_MS.
 * Only fully established sessions enter the worker pool. */

typedef struct https_hs_pending {
    int fd;
    SSL *ssl;
    cwist_https_context *ctx;
    void (*handler)(cwist_https_connection *, void *);
    void *user_ctx;
    uint64_t deadline_ms;
    struct https_hs_pending *next;
} https_hs_pending_t;

static int g_hs_epoll_fd = -1;
static int g_hs_wakeup_rd = -1, g_hs_wakeup_wr = -1;
static pthread_t g_hs_thread;
static bool g_hs_running = false;
static pthread_mutex_t g_hs_lock = PTHREAD_MUTEX_INITIALIZER;
static https_hs_pending_t *g_hs_head = NULL;
static _Atomic long g_hs_pending_count = 0;

static void https_hs_forget_locked(https_hs_pending_t *prev, https_hs_pending_t *p) {
    if (prev) prev->next = p->next;
    else g_hs_head = p->next;
    atomic_fetch_sub_explicit(&g_hs_pending_count, 1, memory_order_release);
}

static void https_hs_abort(https_hs_pending_t *p) {
    if (g_hs_epoll_fd >= 0) epoll_ctl(g_hs_epoll_fd, EPOLL_CTL_DEL, p->fd, NULL);
    SSL_free(p->ssl);
    close(p->fd);
    cwist_free(p);
}

/* Move a completed handshake into the request worker pool. */
static void https_hs_complete(https_hs_pending_t *p) {
    if (g_hs_epoll_fd >= 0) epoll_ctl(g_hs_epoll_fd, EPOLL_CTL_DEL, p->fd, NULL);
    /* The request phase (cwist_https_receive_request & friends) predates
     * non-blocking sockets; restore blocking mode before handing over. */
    int fl = fcntl(p->fd, F_GETFL, 0);
    if (fl >= 0) fcntl(p->fd, F_SETFL, fl & ~O_NONBLOCK);
    https_pool_submit_ready(p->fd, p->ssl, p->ctx, p->handler, p->user_ctx);
    cwist_free(p);
}

static void *https_hs_shepherd(void *arg) {
    (void)arg;
    struct epoll_event events[512];
    while (g_hs_running) {
        int n = epoll_wait(g_hs_epoll_fd, events, 512, 500);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        uint64_t now = cwist_https_now_ms();
        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == NULL) {
                /* wakeup pipe: drain so writers never block */
                char drain[256];
                while (read(g_hs_wakeup_rd, drain, sizeof(drain)) > 0) { /* discard */ }
                continue;
            }
            https_hs_pending_t *p = events[i].data.ptr;

            /* Detach from the list first: every event path below either
             * re-arms (re-add) or finishes with p freed. */
            pthread_mutex_lock(&g_hs_lock);
            https_hs_pending_t *prev = NULL, *cur = g_hs_head;
            bool found = false;
            while (cur) {
                if (cur == p) { https_hs_forget_locked(prev, cur); found = true; break; }
                prev = cur; cur = cur->next;
            }
            pthread_mutex_unlock(&g_hs_lock);
            if (!found) continue; /* already reaped by the sweeper */

            int rc = SSL_accept(p->ssl);
            if (rc > 0) {
                https_hs_complete(p);
                continue;
            }
            int ssl_err = SSL_get_error(p->ssl, rc);
            if ((ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) &&
                now < p->deadline_ms) {
                struct epoll_event ev = {
                    .events = (ssl_err == SSL_ERROR_WANT_WRITE ? EPOLLOUT : EPOLLIN) | EPOLLRDHUP,
                    .data.ptr = p,
                };
                pthread_mutex_lock(&g_hs_lock);
                p->next = g_hs_head;
                g_hs_head = p;
                atomic_fetch_add_explicit(&g_hs_pending_count, 1, memory_order_release);
                pthread_mutex_unlock(&g_hs_lock);
                if (epoll_ctl(g_hs_epoll_fd, EPOLL_CTL_MOD, p->fd, &ev) != 0 &&
                    epoll_ctl(g_hs_epoll_fd, EPOLL_CTL_ADD, p->fd, &ev) != 0) {
                    pthread_mutex_lock(&g_hs_lock);
                    prev = NULL; cur = g_hs_head;
                    while (cur) {
                        if (cur == p) { https_hs_forget_locked(prev, cur); break; }
                        prev = cur; cur = cur->next;
                    }
                    pthread_mutex_unlock(&g_hs_lock);
                    https_hs_abort(p);
                }
                continue;
            }
            https_hs_abort(p); /* hard failure or deadline exceeded */
        }

        /* Sweep expired handshakes that never became readable. */
        uint64_t sweep_now = cwist_https_now_ms();
        pthread_mutex_lock(&g_hs_lock);
        https_hs_pending_t *prev = NULL, *cur = g_hs_head;
        while (cur) {
            https_hs_pending_t *next = cur->next;
            if (sweep_now >= cur->deadline_ms) {
                https_hs_forget_locked(prev, cur);
                pthread_mutex_unlock(&g_hs_lock);
                https_hs_abort(cur);
                pthread_mutex_lock(&g_hs_lock);
                cur = prev ? prev->next : g_hs_head;
                continue;
            }
            prev = cur;
            cur = next;
        }
        pthread_mutex_unlock(&g_hs_lock);
    }
    return NULL;
}

int https_hs_shepherd_start(void) {
    pthread_mutex_lock(&g_hs_lock);
    if (g_hs_running) {
        pthread_mutex_unlock(&g_hs_lock);
        return 0;
    }
    g_hs_epoll_fd = epoll_create1(0);
    if (g_hs_epoll_fd < 0) {
        pthread_mutex_unlock(&g_hs_lock);
        return -1;
    }
    int pfd[2];
    if (pipe(pfd) != 0) {
        close(g_hs_epoll_fd);
        g_hs_epoll_fd = -1;
        pthread_mutex_unlock(&g_hs_lock);
        return -1;
    }
    g_hs_wakeup_rd = pfd[0];
    g_hs_wakeup_wr = pfd[1];
    fcntl(g_hs_wakeup_rd, F_SETFL, O_NONBLOCK);
    fcntl(g_hs_wakeup_wr, F_SETFL, O_NONBLOCK);
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = NULL };
    epoll_ctl(g_hs_epoll_fd, EPOLL_CTL_ADD, g_hs_wakeup_rd, &ev);
    g_hs_running = true;
    if (pthread_create(&g_hs_thread, NULL, https_hs_shepherd, NULL) != 0) {
        g_hs_running = false;
        close(g_hs_wakeup_rd);
        close(g_hs_wakeup_wr);
        close(g_hs_epoll_fd);
        g_hs_epoll_fd = -1;
        pthread_mutex_unlock(&g_hs_lock);
        return -1;
    }
    pthread_mutex_unlock(&g_hs_lock);
    return 0;
}

void https_hs_shepherd_stop(void) {
    pthread_mutex_lock(&g_hs_lock);
    if (!g_hs_running) {
        pthread_mutex_unlock(&g_hs_lock);
        return;
    }
    g_hs_running = false;
    if (g_hs_wakeup_wr >= 0) {
        char b = 1;
        if (write(g_hs_wakeup_wr, &b, 1) < 0) { /* shutdown path */ }
    }
    pthread_mutex_unlock(&g_hs_lock);
    pthread_join(g_hs_thread, NULL);

    pthread_mutex_lock(&g_hs_lock);
    https_hs_pending_t *cur = g_hs_head;
    g_hs_head = NULL;
    pthread_mutex_unlock(&g_hs_lock);
    while (cur) {
        https_hs_pending_t *next = cur->next;
        SSL_free(cur->ssl);
        close(cur->fd);
        cwist_free(cur);
        cur = next;
    }
    if (g_hs_epoll_fd >= 0) close(g_hs_epoll_fd);
    if (g_hs_wakeup_rd >= 0) close(g_hs_wakeup_rd);
    if (g_hs_wakeup_wr >= 0) close(g_hs_wakeup_wr);
    g_hs_epoll_fd = -1;
    g_hs_wakeup_rd = g_hs_wakeup_wr = -1;
}

long cwist_https_pending_handshakes(void) {
    return atomic_load_explicit(&g_hs_pending_count, memory_order_acquire);
}

/**
 * @brief Non-blocking TLS accept: complete the handshake inline when the
 * ClientHello is already here (the common case — the kernel ACKed it while
 * the connection waited in queue), otherwise park it with the shepherd.
 * Safe to call from any thread, including reactor callbacks.
 */
void cwist_https_dispatch(int client_fd, cwist_https_context *ctx, void (*handler)(cwist_https_connection *, void *), void *user_ctx) {
    if (!ctx || !ctx->ctx || client_fd < 0) {
        if (client_fd >= 0) close(client_fd);
        return;
    }
    if (https_hs_shepherd_start() != 0) {
        /* No shepherd, no dispatch: fall back to the blocking pool path. */
        https_pool_submit(client_fd, ctx, handler, user_ctx);
        return;
    }

    int fl = fcntl(client_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(client_fd, F_SETFL, fl | O_NONBLOCK);

    SSL *ssl = SSL_new(ctx->ctx);
    if (!ssl) {
        close(client_fd);
        return;
    }
    SSL_set_fd(ssl, client_fd);

    int rc = SSL_accept(ssl);
    if (rc > 0) {
        int flags = fcntl(client_fd, F_GETFL, 0);
        if (flags >= 0) fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK);
        https_pool_submit_ready(client_fd, ssl, ctx, handler, user_ctx);
        return;
    }
    int ssl_err = SSL_get_error(ssl, rc);
    if (ssl_err != SSL_ERROR_WANT_READ && ssl_err != SSL_ERROR_WANT_WRITE) {
        SSL_free(ssl);
        close(client_fd);
        return;
    }

    https_hs_pending_t *p = cwist_alloc(sizeof(*p));
    if (!p) {
        SSL_free(ssl);
        close(client_fd);
        return;
    }
    p->fd = client_fd;
    p->ssl = ssl;
    p->ctx = ctx;
    p->handler = handler;
    p->user_ctx = user_ctx;
    p->deadline_ms = cwist_https_now_ms() + CWIST_HTTPS_HANDSHAKE_TIMEOUT_MS;

    struct epoll_event ev = {
        .events = (ssl_err == SSL_ERROR_WANT_WRITE ? EPOLLOUT : EPOLLIN) | EPOLLRDHUP,
        .data.ptr = p,
    };
    pthread_mutex_lock(&g_hs_lock);
    p->next = g_hs_head;
    g_hs_head = p;
    atomic_fetch_add_explicit(&g_hs_pending_count, 1, memory_order_release);
    pthread_mutex_unlock(&g_hs_lock);
    if (epoll_ctl(g_hs_epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) != 0) {
        pthread_mutex_lock(&g_hs_lock);
        https_hs_pending_t *prev = NULL, *cur = g_hs_head;
        while (cur) {
            if (cur == p) { https_hs_forget_locked(prev, cur); break; }
            prev = cur; cur = cur->next;
        }
        pthread_mutex_unlock(&g_hs_lock);
        SSL_free(ssl);
        close(client_fd);
        cwist_free(p);
        return;
    }
    /* Wake the shepherd so near-expiry deadlines are honored promptly. */
    char b = 1;
    if (write(g_hs_wakeup_wr, &b, 1) < 0) { /* non-fatal */ }
}

#define CWIST_ALPN_HTTP11       ((const unsigned char *)"\x08http/1.1")
#define CWIST_ALPN_H2_HTTP11    ((const unsigned char *)"\x02h2\x08http/1.1")
#define CWIST_ALPN_H3_H2_HTTP11 ((const unsigned char *)"\x02h3\x02h2\x08http/1.1")
#define CWIST_ALPN_HTTP11_LEN       9
#define CWIST_ALPN_H2_HTTP11_LEN    12
#define CWIST_ALPN_H3_H2_HTTP11_LEN 15

/**
 * @file https.c
 * @brief OpenSSL-backed HTTPS accept, receive, send, and server-loop helpers.
 */

/**
 * @brief Build a JSON-rich cwist_error_t from the latest OpenSSL error state.
 * @param msg Human-readable message describing the failing HTTPS step.
 * @return Error object with module, message, and OpenSSL error string fields.
 */
static cwist_error_t make_ssl_error(const char *msg) {
    cwist_error_t err = make_error(CWIST_ERR_JSON);
    err.error.err_json = cJSON_CreateObject();
    
    unsigned long ssl_err = ERR_get_error();
    char buf[256];
    ERR_error_string_n(ssl_err, buf, sizeof(buf));
    
    cJSON_AddStringToObject(err.error.err_json, "module", "https");
    cJSON_AddStringToObject(err.error.err_json, "message", msg);
    cJSON_AddStringToObject(err.error.err_json, "openssl_error", buf);
    
    return err;
}

/**
 * @brief Initialize OpenSSL and create a server TLS context from PEM files.
 * @param ctx Output pointer that receives the allocated HTTPS context.
 * @param cert_path Path to the PEM certificate chain.
 * @param key_path Path to the PEM private key.
 * @return Tagged CWIST error describing success or failure.
 */
static void cwist_https_apply_base_tls_defaults(SSL_CTX *ssl_ctx) {
    SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_COMPRESSION);
#ifdef SSL_OP_NO_RENEGOTIATION
    SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_RENEGOTIATION);
#endif
    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);

    /* Session resumption. TLS 1.3 tickets are stateless and already on by
     * default; this additionally enables the server-side session cache for
     * TLS 1.2 session IDs and sets the session-id context without which
     * OpenSSL refuses to resume cached sessions. Combined with HTTP/1.1
     * keep-alive this removes most full (asymmetric) handshakes. */
    SSL_CTX_set_session_cache_mode(ssl_ctx, SSL_SESS_CACHE_SERVER);
    SSL_CTX_set_session_id_context(ssl_ctx, (const unsigned char *)"cwist-v1", 8);
    SSL_CTX_set_timeout(ssl_ctx, 300);
}

static cwist_error_t cwist_https_apply_http2_tls_profile(SSL_CTX *ssl_ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (SSL_CTX_set_cipher_list(ssl_ctx,
                                "ECDHE-ECDSA-AES128-GCM-SHA256:"
                                "ECDHE-RSA-AES128-GCM-SHA256:"
                                "ECDHE-ECDSA-AES256-GCM-SHA384:"
                                "ECDHE-RSA-AES256-GCM-SHA384:"
                                "ECDHE-ECDSA-CHACHA20-POLY1305:"
                                "ECDHE-RSA-CHACHA20-POLY1305") != 1) {
        return make_ssl_error("Unable to apply HTTP/2-compatible TLS 1.2 cipher profile");
    }
#ifdef SSL_CTX_set_ciphersuites
    if (SSL_CTX_set_ciphersuites(ssl_ctx,
                                 "TLS_AES_128_GCM_SHA256:"
                                 "TLS_AES_256_GCM_SHA384:"
                                 "TLS_CHACHA20_POLY1305_SHA256") != 1) {
        return make_ssl_error("Unable to apply HTTP/2-compatible TLS 1.3 cipher suites");
    }
#endif
    SSL_CTX_set_options(ssl_ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
    err.error.err_i16 = 0;
    return err;
}

static int cwist_https_alpn_select_cb(SSL *ssl,
                                      const unsigned char **out,
                                      unsigned char *outlen,
                                      const unsigned char *in,
                                      unsigned int inlen,
                                      void *arg) {
    (void)ssl;
    const cwist_https_context *hctx = (const cwist_https_context *)arg;
    bool enable_http2 = hctx && hctx->http2_enabled;

    const unsigned char *supported;
    unsigned int supported_len;

    if (enable_http2) {
        supported = CWIST_ALPN_H2_HTTP11;
        supported_len = CWIST_ALPN_H2_HTTP11_LEN;
    } else {
        supported = CWIST_ALPN_HTTP11;
        supported_len = CWIST_ALPN_HTTP11_LEN;
    }

    if (SSL_select_next_proto((unsigned char **)out,
                              outlen,
                              supported,
                              supported_len,
                              in,
                              inlen) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }

    return SSL_TLSEXT_ERR_NOACK;
}

/* --- Context Management --- */

/* --- Shared session ticket key -------------------------------------------
 * cwist_app_listen() forks one worker per CPU core. BoringSSL/OpenSSL
 * generate their internal session-ticket keys lazily on first use, i.e.
 * AFTER the fork, so every worker would mint its own key and tickets issued
 * by one worker could never be resumed on another (SO_REUSEPORT scatters
 * connections across workers). Instead we generate one random key at
 * context creation (in the parent, pre-fork) and install an explicit
 * ticket-key callback; every forked worker inherits identical key material
 * and tickets resume regardless of which worker accepts the connection.
 *
 * Trade-off: the key lives for the process lifetime (no rotation), which is
 * the standard pre-fork server model (same as a fixed nginx ticket key).
 * ------------------------------------------------------------------------- */
typedef struct cwist_tls_ticket_key {
    unsigned char name[16];     /* key_name sent in the ticket */
    unsigned char aes_key[32];  /* AES-256-CBC encryption key  */
    unsigned char hmac_key[32]; /* HMAC-SHA-256 MAC key        */
} cwist_tls_ticket_key;

static int g_ticket_key_ex_data_idx = -1;
static pthread_once_t g_ticket_key_ex_data_once = PTHREAD_ONCE_INIT;

static void cwist_tls_ticket_key_ex_data_init(void) {
    g_ticket_key_ex_data_idx = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}

static int cwist_tls_ticket_key_cb(SSL *ssl, uint8_t *key_name, uint8_t *iv,
                                   EVP_CIPHER_CTX *ectx, HMAC_CTX *hctx, int encrypt) {
    SSL_CTX *ssl_ctx = ssl ? SSL_get_SSL_CTX(ssl) : NULL;
    const cwist_tls_ticket_key *key = NULL;
    if (ssl_ctx && g_ticket_key_ex_data_idx >= 0) {
        key = (const cwist_tls_ticket_key *)SSL_CTX_get_ex_data(ssl_ctx, g_ticket_key_ex_data_idx);
    }
    if (!key) return 0;

    if (encrypt) {
        memcpy(key_name, key->name, sizeof(key->name));
        if (RAND_bytes(iv, 16) != 1) return 0;
        if (EVP_EncryptInit_ex(ectx, EVP_aes_256_cbc(), NULL, key->aes_key, iv) != 1) return 0;
        if (HMAC_Init_ex(hctx, key->hmac_key, sizeof(key->hmac_key), EVP_sha256(), NULL) != 1) return 0;
        return 1;
    }

    if (memcmp(key_name, key->name, sizeof(key->name)) != 0) {
        return 0; /* Unknown key: decline the ticket, do a full handshake. */
    }
    if (EVP_DecryptInit_ex(ectx, EVP_aes_256_cbc(), NULL, key->aes_key, iv) != 1) return 0;
    if (HMAC_Init_ex(hctx, key->hmac_key, sizeof(key->hmac_key), EVP_sha256(), NULL) != 1) return 0;
    return 1;
}

/**
 * @brief Generate the shared ticket key and arm the ticket-key callback.
 * @param ssl_ctx Context being configured.
 * @param out_key Receives ownership of the key material (freed by the caller).
 */
static void cwist_tls_setup_shared_ticket_key(SSL_CTX *ssl_ctx, void **out_key) {
    *out_key = NULL;
    pthread_once(&g_ticket_key_ex_data_once, cwist_tls_ticket_key_ex_data_init);
    if (g_ticket_key_ex_data_idx < 0) return;

    cwist_tls_ticket_key *key = (cwist_tls_ticket_key *)cwist_alloc(sizeof(*key));
    if (!key) return;
    if (RAND_bytes((uint8_t *)key, sizeof(*key)) != 1) {
        cwist_free(key);
        return; /* Fall back to the library-internal (per-worker) keys. */
    }
    SSL_CTX_set_ex_data(ssl_ctx, g_ticket_key_ex_data_idx, key);
    SSL_CTX_set_tlsext_ticket_key_cb(ssl_ctx, cwist_tls_ticket_key_cb);
    *out_key = key;
}

cwist_error_t cwist_https_init_context(cwist_https_context **ctx, const char *cert_path, const char *key_path) {
    return cwist_https_init_context_with_options(ctx, cert_path, key_path, NULL, NULL);
}

cwist_error_t cwist_https_init_context_with_options(cwist_https_context **ctx,
                                                    const char *cert_path,
                                                    const char *key_path,
                                                    const cwist_https_options *options,
                                                    cwist_app *app) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    bool enable_http3 = options && options->enable_http3;
    bool enable_http2 = options && (options->enable_http2 || enable_http3);
    
    if (!ctx || !cert_path || !key_path) {
        err.error.err_i16 = -1;
        return err;
    }

    // Initialize OpenSSL
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    const SSL_METHOD *method = TLS_server_method();
    SSL_CTX *ssl_ctx = SSL_CTX_new(method);
    if (!ssl_ctx) {
        return make_ssl_error("Unable to create SSL context");
    }

    if (SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION) != 1) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to enforce TLS minimum version");
    }
    cwist_https_apply_base_tls_defaults(ssl_ctx);

    if (!cwist_tls_apply_pqc_layer(app, ssl_ctx)) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("PQC layer configuration failed");
    }

    if (enable_http2) {
        err = cwist_https_apply_http2_tls_profile(ssl_ctx);
        if (err.errtype != CWIST_ERR_INT16 || err.error.err_i16 != 0) {
            SSL_CTX_free(ssl_ctx);
            return err;
        }
    }

    // Load Cert and Key
    if (SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_path) <= 0) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to load certificate");
    }

    if (cwist_tls_autoload_intermediates(ssl_ctx) < 0) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to complete certificate chain");
    }

    if (SSL_CTX_use_PrivateKey_file(ssl_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Unable to load private key");
    }

    // Verify key matches cert
    if (!SSL_CTX_check_private_key(ssl_ctx)) {
        SSL_CTX_free(ssl_ctx);
        return make_ssl_error("Private key does not match certificate");
    }

    *ctx = (cwist_https_context*)cwist_alloc(sizeof(cwist_https_context));
    if (!*ctx) {
        SSL_CTX_free(ssl_ctx);
        err.error.err_i16 = -1;
        return err;
    }
    (*ctx)->ctx = ssl_ctx;
    (*ctx)->http2_enabled = enable_http2;
    (*ctx)->http3_enabled = enable_http3;
    (*ctx)->ticket_key = NULL;

    /* Pre-fork shared ticket key so every forked worker can resume tickets
     * issued by any other worker. Must run before workers are forked. */
    cwist_tls_setup_shared_ticket_key(ssl_ctx, &(*ctx)->ticket_key);

    SSL_CTX_set_alpn_select_cb(ssl_ctx,
                               cwist_https_alpn_select_cb,
                               (void *)*ctx);

    err.error.err_i16 = 0; // Success
    return err;
}

/**
 * @brief Free an HTTPS context and release its OpenSSL resources.
 * @param ctx Context to destroy.
 */
void cwist_https_destroy_context(cwist_https_context *ctx) {
    if (ctx) {
        if (ctx->ctx) {
            SSL_CTX_free(ctx->ctx);
        }
        if (ctx->ticket_key) {
            /* Scrub key material before releasing it. */
            OPENSSL_cleanse(ctx->ticket_key, sizeof(cwist_tls_ticket_key));
            cwist_free(ctx->ticket_key);
        }
        cwist_free(ctx);
        /* EVP_cleanup() intentionally not called: it is deprecated since
         * OpenSSL 1.1.0 and tears down global state other code may use. */
    }
}

/**
 * @brief Wrap an accepted TCP client socket in an OpenSSL connection object.
 * @param ctx HTTPS context holding the configured SSL_CTX.
 * @param client_fd Accepted TCP socket descriptor.
 * @param conn Output pointer that receives the allocated connection wrapper.
 * @return Tagged CWIST error describing success or failure.
 */
/* Build the connection wrapper around an already-established TLS session.
 * Shared by the blocking cwist_https_accept path and the shepherd's
 * non-blocking dispatch path. */
static cwist_error_t https_wrap_established(cwist_https_context *ctx, int client_fd, SSL *ssl, cwist_https_connection **conn) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    *conn = (cwist_https_connection*)cwist_alloc(sizeof(cwist_https_connection));
    if (!*conn) {
        err.error.err_i16 = -1;
        return err;
    }

    (*conn)->fd = client_fd;
    (*conn)->ssl = ssl;
    (*conn)->read_buf = cwist_alloc(CWIST_HTTP_READ_BUFFER_SIZE);
    if (!(*conn)->read_buf) {
        cwist_free(*conn);
        *conn = NULL;
        err.error.err_i16 = -1;
        return err;
    }
    (*conn)->buf_len = 0;
    (*conn)->read_buf[0] = '\0';
    (*conn)->negotiated_http2 = false;
    (*conn)->negotiated_protocol = CWIST_HTTPS_PROTOCOL_HTTP11;
    (*conn)->http3_enabled = ctx->http3_enabled;

    const unsigned char *alpn = NULL;
    unsigned int alpn_len = 0;
    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
    if (alpn && alpn_len == 2 && memcmp(alpn, "h2", 2) == 0) {
        (*conn)->negotiated_http2 = true;
        (*conn)->negotiated_protocol = CWIST_HTTPS_PROTOCOL_HTTP2;
    }
    /* h3 is QUIC-only and never negotiated over TCP TLS */

    err.error.err_i16 = 0;
    return err;
}

cwist_error_t cwist_https_accept(cwist_https_context *ctx, int client_fd, cwist_https_connection **conn) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (!ctx || !ctx->ctx || client_fd < 0) {
        err.error.err_i16 = -1;
        return err;
    }

    SSL *ssl = SSL_new(ctx->ctx);
    if (!ssl) {
        return make_ssl_error("Failed to create SSL structure");
    }

    SSL_set_fd(ssl, client_fd);

    /* Bound the whole handshake so a client dribbling bytes cannot pin a
     * pool worker forever. */
    uint64_t handshake_deadline = cwist_https_now_ms() + CWIST_HTTPS_HANDSHAKE_TIMEOUT_MS;
    int rc;
    while ((rc = SSL_accept(ssl)) <= 0) {
        int ssl_err = SSL_get_error(ssl, rc);
        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
            uint64_t now = cwist_https_now_ms();
            if (now >= handshake_deadline) {
                cwist_error_t err_obj = make_ssl_error("SSL handshake timed out");
                SSL_free(ssl);
                return err_obj;
            }
            int wait_ms = CWIST_HTTP_TIMEOUT_MS;
            uint64_t remaining = handshake_deadline - now;
            if (remaining < (uint64_t)wait_ms) wait_ms = (int)remaining;
            if (cwist_ssl_wait(client_fd, ssl_err, wait_ms) != 0) {
                cwist_error_t err_obj = make_ssl_error("SSL handshake timed out or socket error");
                SSL_free(ssl);
                return err_obj;
            }
            continue;
        }
        cwist_error_t err_obj = make_ssl_error("SSL handshake failed");
        SSL_free(ssl);
        return err_obj;
    }

    cwist_error_t wrap_err = https_wrap_established(ctx, client_fd, ssl, conn);
    if (wrap_err.errtype == CWIST_ERR_INT16 && wrap_err.error.err_i16 != 0) {
        SSL_free(ssl);
        return wrap_err;
    }
    return wrap_err;
}

/**
 * @brief Gracefully close an HTTPS connection and free its buffers.
 * @param conn HTTPS connection wrapper to close.
 */
bool cwist_https_connection_uses_http2(const cwist_https_connection *conn) {
    return conn && conn->negotiated_protocol == CWIST_HTTPS_PROTOCOL_HTTP2;
}

cwist_https_protocol cwist_https_connection_protocol(const cwist_https_connection *conn) {
    if (!conn) return CWIST_HTTPS_PROTOCOL_NONE;
    return conn->negotiated_protocol;
}

void cwist_https_close_connection(cwist_https_connection *conn) {
    if (conn) {
        if (conn->ssl) {
            SSL_shutdown(conn->ssl);
            SSL_free(conn->ssl);
        }
        if (conn->fd >= 0) {
            close(conn->fd);
        }
        cwist_free(conn->read_buf);
        cwist_free(conn);
    }
}

/**
 * @brief Read from the TLS stream until a full HTTP request has been assembled.
 * @param conn Active HTTPS connection wrapper.
 * @return Parsed HTTP request, or NULL on timeout, parse failure, or IO failure.
 */
cwist_http_request *cwist_https_receive_request(cwist_https_connection *conn) {
    if (!conn || !conn->ssl || !conn->read_buf) return NULL;

    size_t total_received = conn->buf_len;
    char *header_end = NULL;

    /* Total deadline for assembling the request headers. */
    uint64_t headers_deadline = cwist_https_now_ms() + CWIST_HTTP_HEADERS_TIMEOUT_MS;

    /* Rescanning from offset 0 on every read is O(n^2) over dribbled
     * headers; resume 3 bytes back so a terminator split across two reads
     * is still found. */
    size_t scan_from = 0;

    while (1) {
        header_end = strstr(conn->read_buf + scan_from, "\r\n\r\n");
        if (header_end) break;
        scan_from = total_received > 3 ? total_received - 3 : 0;

        if (total_received >= CWIST_HTTP_READ_BUFFER_SIZE - 1) {
            return NULL;
        }

        if (SSL_pending(conn->ssl) == 0) {
            uint64_t now = cwist_https_now_ms();
            if (now >= headers_deadline) {
                return NULL;
            }
            int wait_ms = CWIST_HTTP_TIMEOUT_MS;
            uint64_t remaining = headers_deadline - now;
            if (remaining < (uint64_t)wait_ms) wait_ms = (int)remaining;

            struct pollfd pfd = { .fd = conn->fd, .events = POLLIN };
            int pret = poll(&pfd, 1, wait_ms);
            if (pret <= 0) {
                return NULL;
            }
        }

        int bytes = SSL_read(conn->ssl, conn->read_buf + total_received, (int)(CWIST_HTTP_READ_BUFFER_SIZE - 1 - total_received));
        if (bytes <= 0) {
            int ssl_err = SSL_get_error(conn->ssl, bytes);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                if (cwist_ssl_wait(conn->fd, ssl_err, CWIST_HTTP_TIMEOUT_MS) != 0) return NULL;
                continue;
            }
            return NULL;
        }

        total_received += (size_t)bytes;
        conn->read_buf[total_received] = '\0';
    }

    cwist_http_request *req = cwist_http_parse_request(conn->read_buf);
    if (!req) return NULL;

    req->client_fd = conn->fd;

    size_t header_len = (header_end + 4) - conn->read_buf;
    size_t body_received = total_received - header_len;

    if (req->content_length > 0) {
        if (req->content_length > CWIST_HTTP_MAX_BODY_SIZE) {
            cwist_http_request_destroy(req);
            return NULL;
        }

        char *body = cwist_alloc(req->content_length + 1);
        if (!body) {
            cwist_http_request_destroy(req);
            return NULL;
        }

        size_t to_copy = body_received < req->content_length ? body_received : req->content_length;
        memcpy(body, header_end + 4, to_copy);
        size_t current_body_len = to_copy;

        /* No total cap (slow 1 GiB uploads must keep working); abort only
         * when no bytes arrive for a cumulative idle span.  Any successful
         * read resets the idle clock. */
        uint64_t body_idle_start = cwist_https_now_ms();

        while (current_body_len < req->content_length) {
            if (SSL_pending(conn->ssl) == 0) {
                uint64_t now = cwist_https_now_ms();
                if (now - body_idle_start >= CWIST_HTTP_BODY_IDLE_TIMEOUT_MS) {
                    cwist_free(body);
                    cwist_http_request_destroy(req);
                    return NULL;
                }
                int wait_ms = CWIST_HTTP_TIMEOUT_MS;
                uint64_t idle_left = CWIST_HTTP_BODY_IDLE_TIMEOUT_MS - (now - body_idle_start);
                if (idle_left < (uint64_t)wait_ms) wait_ms = (int)idle_left;

                struct pollfd pfd = { .fd = conn->fd, .events = POLLIN };
                int pret = poll(&pfd, 1, wait_ms);
                if (pret <= 0) {
                    cwist_free(body);
                    cwist_http_request_destroy(req);
                    return NULL;
                }
            }

            int bytes = SSL_read(conn->ssl, body + current_body_len, (int)(req->content_length - current_body_len));
            if (bytes <= 0) {
                int ssl_err = SSL_get_error(conn->ssl, bytes);
                if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                    if (cwist_ssl_wait(conn->fd, ssl_err, CWIST_HTTP_TIMEOUT_MS) != 0) {
                        cwist_free(body);
                        cwist_http_request_destroy(req);
                        return NULL;
                    }
                    continue;
                }
                cwist_free(body);
                cwist_http_request_destroy(req);
                return NULL;
            }
            current_body_len += (size_t)bytes;
            body_idle_start = cwist_https_now_ms();
        }
        body[req->content_length] = '\0';
        /* Adopt the filled buffer: one allocation, zero copies. */
        cwist_sstring_adopt_len(req->body, body, req->content_length);

        if (body_received > req->content_length) {
            size_t leftover_len = body_received - req->content_length;
            memmove(conn->read_buf, header_end + 4 + req->content_length, leftover_len);
            conn->buf_len = leftover_len;
        } else {
            conn->buf_len = 0;
        }
    } else {
        if (body_received > 0) {
            memmove(conn->read_buf, header_end + 4, body_received);
            conn->buf_len = body_received;
        } else {
            conn->buf_len = 0;
        }
    }
    conn->read_buf[conn->buf_len] = '\0';

    return req;
}

/**
 * @brief Write a full buffer over TLS, retrying through WANT_READ/WANT_WRITE.
 * @param conn Active HTTPS connection wrapper.
 * @param buf Bytes to write.
 * @param len Number of bytes to write.
 * @return 0 on success, -1 on timeout or fatal SSL error.
 */
static int cwist_ssl_write_all(cwist_https_connection *conn, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        /* SSL_write takes an int; cap each call well below INT_MAX. */
        size_t left = len - off;
        int chunk = left > 0x3fffffff ? 0x3fffffff : (int)left;
        int sent = SSL_write(conn->ssl, buf + off, chunk);
        if (sent <= 0) {
            int ssl_err = SSL_get_error(conn->ssl, sent);
            if (ssl_err == SSL_ERROR_WANT_WRITE || ssl_err == SSL_ERROR_WANT_READ) {
                if (cwist_ssl_wait(conn->fd, ssl_err, CWIST_HTTP_TIMEOUT_MS) != 0) {
                    return -1;
                }
                continue;
            }
            return -1;
        }
        off += (size_t)sent;
    }
    return 0;
}

/**
 * @brief Serialize an HTTP response and send it over an active TLS connection.
 *
 * Zero-copy counterpart of the plaintext send path: the header block is
 * serialized into a stack buffer and written first, then the body is
 * streamed straight from its origin (pointer body, sstring, or file stream)
 * instead of materializing one contiguous heap blob.
 *
 * @param conn Active HTTPS connection wrapper.
 * @param res Response object to serialize.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_https_send_response(cwist_https_connection *conn, cwist_http_response *res) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (!conn || !conn->ssl || !res) {
        err.error.err_i16 = -1;
        return err;
    }

    // Inject Alt-Svc when HTTP/3 is enabled so clients discover the QUIC endpoint
    if (conn->http3_enabled) {
        struct sockaddr_storage ss;
        socklen_t ss_len = sizeof(ss);
        int port = 443;
        if (getsockname(conn->fd, (struct sockaddr *)&ss, &ss_len) == 0) {
            if (ss.ss_family == AF_INET) {
                port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
            } else if (ss.ss_family == AF_INET6) {
                port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
            }
        }
        char alt_svc[64];
        snprintf(alt_svc, sizeof(alt_svc), "h3=\":%d\"; ma=86400", port);
        cwist_http_header_add(&res->headers, "Alt-Svc", alt_svc);
    }

    // 1. Headers onto a stack buffer, then straight onto the wire
    char header_buf[CWIST_HTTP_MAX_HEADER_SIZE];
    size_t header_len = cwist_http_serialize_headers(res, header_buf, sizeof(header_buf));
    if (cwist_ssl_write_all(conn, header_buf, header_len) != 0) {
        return make_ssl_error("SSL header write failed");
    }

    // 2. Body streamed from its origin; no intermediate serialization
    const char *body_ptr = NULL;
    size_t body_len = 0;
    if (res->is_ptr_body) {
        body_ptr = (const char *)res->ptr_body;
        body_len = res->ptr_body_len;
    } else if (res->body && res->body->data) {
        body_ptr = res->body->data;
        body_len = res->body->size;
    }
    if (body_ptr && body_len > 0) {
        if (cwist_ssl_write_all(conn, body_ptr, body_len) != 0) {
            return make_ssl_error("SSL body write failed");
        }
    }

    // 3. File streams cannot use sendfile() through userland TLS; chunk them
    if (res->use_file_stream && res->file_stream_fd >= 0) {
        char fbuf[65536];
        size_t remaining = res->file_stream_len;
        off_t offset = res->file_stream_offset;
        while (remaining > 0) {
            size_t to_read = remaining < sizeof(fbuf) ? remaining : sizeof(fbuf);
            ssize_t n = pread(res->file_stream_fd, fbuf, to_read, offset);
            if (n < 0) {
                if (errno == EINTR) continue;
                return make_ssl_error("file stream read failed");
            }
            if (n == 0) break;
            if (cwist_ssl_write_all(conn, fbuf, (size_t)n) != 0) {
                return make_ssl_error("SSL file stream write failed");
            }
            offset += n;
            remaining -= (size_t)n;
        }
        res->file_stream_offset = offset;
    }

    err.error.err_i16 = 0;
    return err;
}

/**
 * @brief Worker entry point that performs the TLS handshake before dispatching.
 * @param arg Thread payload containing the accepted socket and dispatch callback.
 * @return Always NULL for pthread compatibility.
 */
static void *https_thread_handler(void *arg) {
    struct https_thread_payload *payload = (struct https_thread_payload *)arg;
    cwist_https_connection *conn = NULL;
    cwist_error_t hs_err;

    if (payload->pres_ssl) {
        /* Handshake already completed by the shepherd thread. */
        hs_err = https_wrap_established(payload->ctx, payload->client_fd, payload->pres_ssl, &conn);
        if (!(hs_err.errtype == CWIST_ERR_INT16 && hs_err.error.err_i16 == 0)) {
            SSL_free(payload->pres_ssl);
        }
    } else {
        hs_err = cwist_https_accept(payload->ctx, payload->client_fd, &conn);
    }

    if (hs_err.errtype == CWIST_ERR_INT16 && hs_err.error.err_i16 == 0) {
        payload->handler(conn, payload->user_ctx);
        cwist_https_close_connection(conn);
    } else {
        if (hs_err.errtype == CWIST_ERR_JSON) {
            cJSON_Delete(hs_err.error.err_json);
        }
        close(payload->client_fd);
    }

    cwist_free(payload);
    return NULL;
}

/**
 * @brief Accept HTTPS clients in a loop and dispatch each one to the supplied handler.
 * @param server_fd Bound listening socket descriptor.
 * @param ctx HTTPS context shared by all accepted connections.
 * @param handler Callback invoked for each successful TLS client wrapper.
 * @param user_ctx Opaque pointer forwarded to the handler.
 * @return Tagged CWIST error describing success or failure.
 */
cwist_error_t cwist_https_server_loop(int server_fd, cwist_https_context *ctx, void (*handler)(cwist_https_connection *, void *), void *user_ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (server_fd < 0 || !ctx || !handler) {
        err.error.err_i16 = -1;
        return err;
    }

    if (https_pool_init() != 0) {
        err.error.err_i16 = -1;
        return err;
    }

    while (atomic_load(&g_cwist_running)) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&addr, &len);

        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EBADF || errno == EINVAL) break;
            continue;
        }

        /* Reap vanished peers within ~2 minutes instead of the ~2h kernel
         * default, so dead connections cannot park pool workers forever. */
        {
            int one = 1;
            setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
            int keepidle = 60;
            setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
#endif
#ifdef TCP_KEEPINTVL
            int keepintvl = 10;
            setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
#endif
#ifdef TCP_KEEPCNT
            int keepcnt = 6;
            setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
#endif
        }

        cwist_https_dispatch(client_fd, ctx, handler, user_ctx);
    }
    
    https_pool_destroy();
    return err;
}
