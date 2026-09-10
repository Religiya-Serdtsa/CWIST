#define _POSIX_C_SOURCE 200809L
/* gRPC channel tests: resolver, pick_first/round_robin load balancing, and
 * the gRFC A6 retry engine (retryable codes, backoff, pushback, throttling,
 * deadlines, transparent retries) over real h2c loopback connections. */
#include <cwist/sys/app/app.h>
#include <cwist/net/grpc/grpc.h>
#include <cwist/net/grpc/grpc_channel.h>
#include <cwist/net/http/http2.h>
#include <cwist/net/http/https.h>
#include <cwist/core/mem/alloc.h>
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* --- backends: two loopback h2c servers, one thread per connection --- */

#define MAX_CONN_THREADS 32

typedef struct {
    char id;                        /* 'A' or 'B', embedded in responses */
    atomic_int calls;               /* total unary calls seen */
    atomic_int flaky_failures;      /* UNAVAILABLEs left before succeeding */
    atomic_int pushback_failures;   /* UNAVAILABLE+pushback left before succeeding */
    int pushback_ms;                /* grpc-retry-pushback-ms value */
} backend_state;

typedef struct {
    cwist_app *app;
    backend_state state;
    int listen_fd;
    pthread_t accept_tid;
    pthread_t conn_tids[MAX_CONN_THREADS];
    int conn_count;
    pthread_mutex_t conn_mu;
} test_backend;

static void bk_success(cwist_http_request *req, cwist_http_response *res,
                       backend_state *bs) {
    const char *pa = cwist_grpc_metadata_get(req, "grpc-previous-rpc-attempts");
    char text[64];
    snprintf(text, sizeof(text), "%c:%s", bs->id, pa ? pa : "0");
    cwist_pb_writer w;
    cwist_pb_writer_init(&w);
    assert(cwist_pb_write_string_field(&w, 1, text) == 0);
    cwist_grpc_set_response(res, CWIST_GRPC_OK, NULL, w.data, w.len);
    cwist_pb_writer_free(&w);
}

static void bk_who(cwist_http_request *req, cwist_http_response *res,
                   const cwist_grpc_message *message, void *user_ctx) {
    (void)message;
    backend_state *bs = user_ctx;
    atomic_fetch_add(&bs->calls, 1);
    bk_success(req, res, bs);
}

static void bk_flaky(cwist_http_request *req, cwist_http_response *res,
                     const cwist_grpc_message *message, void *user_ctx) {
    (void)message;
    backend_state *bs = user_ctx;
    atomic_fetch_add(&bs->calls, 1);
    if (atomic_fetch_sub(&bs->flaky_failures, 1) > 0) {
        cwist_grpc_set_error(res, CWIST_GRPC_UNAVAILABLE, "try again");
        return;
    }
    bk_success(req, res, bs);
}

static void bk_fail(cwist_http_request *req, cwist_http_response *res,
                    const cwist_grpc_message *message, void *user_ctx) {
    (void)req;
    (void)message;
    (void)user_ctx;
    cwist_grpc_set_error(res, CWIST_GRPC_INVALID_ARGUMENT, "bad input");
}

static void bk_always(cwist_http_request *req, cwist_http_response *res,
                      const cwist_grpc_message *message, void *user_ctx) {
    (void)req;
    (void)message;
    backend_state *bs = user_ctx;
    atomic_fetch_add(&bs->calls, 1);
    cwist_grpc_set_error(res, CWIST_GRPC_UNAVAILABLE, "always down");
}

static void bk_pushback(cwist_http_request *req, cwist_http_response *res,
                        const cwist_grpc_message *message, void *user_ctx) {
    (void)message;
    backend_state *bs = user_ctx;
    atomic_fetch_add(&bs->calls, 1);
    if (atomic_fetch_sub(&bs->pushback_failures, 1) > 0) {
        char pb[16];
        snprintf(pb, sizeof(pb), "%d", bs->pushback_ms);
        cwist_http_header_add(&res->headers, "grpc-retry-pushback-ms", pb);
        cwist_grpc_set_error(res, CWIST_GRPC_UNAVAILABLE, "pushback");
        return;
    }
    bk_success(req, res, bs);
}

static void bk_bridge(void *user_ctx, cwist_http_request *req,
                      cwist_http_response *res) {
    cwist_app *app = user_ctx;
    req->app = app;
    cwist_app_dispatch(app, req, res);
}

typedef struct {
    test_backend *bk;
    int fd;
} bk_conn_arg;

static void *bk_conn_thread(void *arg) {
    bk_conn_arg *ca = arg;
    test_backend *bk = ca->bk;
    int fd = ca->fd;
    cwist_free(ca);
    cwist_https_connection conn = {
        .fd = fd,
        .ssl = NULL,
        .read_buf = NULL,
        .buf_len = 0,
        .negotiated_http2 = true,
        .negotiated_protocol = CWIST_HTTPS_PROTOCOL_HTTP2,
        .http2_sequenced_data = false
    };
    cwist_http2_serve_connection_ex(&conn, bk->app, bk_bridge,
                                    cwist_grpc_http2_hooks());
    close(fd);
    return NULL;
}

static void *bk_accept_loop(void *arg) {
    test_backend *bk = arg;
    for (;;) {
        int cfd = accept(bk->listen_fd, NULL, NULL);
        if (cfd < 0) break;
        bk_conn_arg *ca = cwist_alloc(sizeof(*ca));
        assert(ca != NULL);
        ca->bk = bk;
        ca->fd = cfd;
        pthread_t tid;
        assert(pthread_create(&tid, NULL, bk_conn_thread, ca) == 0);
        pthread_mutex_lock(&bk->conn_mu);
        assert(bk->conn_count < MAX_CONN_THREADS);
        bk->conn_tids[bk->conn_count++] = tid;
        pthread_mutex_unlock(&bk->conn_mu);
    }
    return NULL;
}

static uint16_t start_backend(test_backend *bk, char id) {
    memset(bk, 0, sizeof(*bk));
    bk->state.id = id;
    bk->state.pushback_ms = -1;
    pthread_mutex_init(&bk->conn_mu, NULL);
    bk->app = cwist_app_create();
    assert(bk->app != NULL);
    assert(cwist_app_grpc_unary(bk->app, "cwist.test.Bk", "Who", bk_who, &bk->state) == 0);
    assert(cwist_app_grpc_unary(bk->app, "cwist.test.Bk", "Flaky", bk_flaky, &bk->state) == 0);
    assert(cwist_app_grpc_unary(bk->app, "cwist.test.Bk", "Fail", bk_fail, &bk->state) == 0);
    assert(cwist_app_grpc_unary(bk->app, "cwist.test.Bk", "Always", bk_always, &bk->state) == 0);
    assert(cwist_app_grpc_unary(bk->app, "cwist.test.Bk", "Pushback", bk_pushback, &bk->state) == 0);

    bk->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(bk->listen_fd >= 0);
    int one = 1;
    setsockopt(bk->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(bk->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(bk->listen_fd, 16) == 0);
    socklen_t alen = sizeof(addr);
    assert(getsockname(bk->listen_fd, (struct sockaddr *)&addr, &alen) == 0);
    assert(pthread_create(&bk->accept_tid, NULL, bk_accept_loop, bk) == 0);
    return ntohs(addr.sin_port);
}

static void stop_backend(test_backend *bk) {
    shutdown(bk->listen_fd, SHUT_RDWR);
    close(bk->listen_fd);
    pthread_join(bk->accept_tid, NULL);
    pthread_mutex_lock(&bk->conn_mu);
    for (int i = 0; i < bk->conn_count; i++) pthread_join(bk->conn_tids[i], NULL);
    bk->conn_count = 0;
    pthread_mutex_unlock(&bk->conn_mu);
    cwist_app_destroy(bk->app);
    pthread_mutex_destroy(&bk->conn_mu);
}

/* Reserve a port that is guaranteed closed afterwards (connect refuses). */
static uint16_t closed_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t alen = sizeof(addr);
    assert(getsockname(fd, (struct sockaddr *)&addr, &alen) == 0);
    uint16_t port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

/* --- raw GOAWAY fake: speaks just enough HTTP/2 to accept the handshake,
 * then answers the first call with GOAWAY(last_stream_id=0) — gRFC A6
 * transparent-retry case 3.  Accepts exactly one connection. --- */

typedef struct {
    int listen_fd;
    pthread_t tid;
    atomic_int served;
} goaway_fake;

static int fake_read_all(int fd, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, (uint8_t *)buf + got, len - got);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static int fake_write_all(int fd, const void *buf, size_t len) {
    size_t put = 0;
    while (put < len) {
        ssize_t n = write(fd, (const uint8_t *)buf + put, len - put);
        if (n <= 0) return -1;
        put += (size_t)n;
    }
    return 0;
}

static void fake_send_frame(int fd, uint8_t type, uint8_t flags, uint32_t stream_id,
                            const void *payload, uint32_t len) {
    uint8_t hdr[9] = {
        (uint8_t)((len >> 16) & 0xff), (uint8_t)((len >> 8) & 0xff), (uint8_t)(len & 0xff),
        type, flags,
        (uint8_t)((stream_id >> 24) & 0x7f), (uint8_t)((stream_id >> 16) & 0xff),
        (uint8_t)((stream_id >> 8) & 0xff), (uint8_t)(stream_id & 0xff)
    };
    fake_write_all(fd, hdr, sizeof(hdr));
    if (len) fake_write_all(fd, payload, len);
}

static void *goaway_fake_thread(void *arg) {
    goaway_fake *fake = arg;
    int fd = accept(fake->listen_fd, NULL, NULL);
    if (fd < 0) return NULL;
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    atomic_fetch_add(&fake->served, 1);

    uint8_t preface[24];
    if (fake_read_all(fd, preface, sizeof(preface)) != 0) goto out;
    int saw_headers = 0, saw_end_stream = 0;
    for (;;) {
        uint8_t hdr[9];
        if (fake_read_all(fd, hdr, sizeof(hdr)) != 0) goto out;
        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | hdr[2];
        uint8_t type = hdr[3];
        uint8_t flags = hdr[4];
        uint8_t payload[16384];
        if (len > sizeof(payload)) goto out;
        if (len && fake_read_all(fd, payload, len) != 0) goto out;
        if (type == 0x04 && !(flags & 0x01)) {
            /* Answer client SETTINGS: our SETTINGS plus the ACK of theirs. */
            fake_send_frame(fd, 0x04, 0, 0, NULL, 0);
            fake_send_frame(fd, 0x04, 0x01, 0, NULL, 0);
        } else if (type == 0x01) {
            saw_headers = 1;
            if (flags & 0x01) saw_end_stream = 1;
        } else if (type == 0x00 && (flags & 0x01)) {
            saw_end_stream = 1;
        }
        if (saw_headers && saw_end_stream) {
            /* GOAWAY with last_stream_id 0: stream 1 was never seen by any
             * server application logic. */
            uint8_t goaway[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
            fake_send_frame(fd, 0x07, 0, 0, goaway, sizeof(goaway));
            struct timespec ts = { 0, 100000000L };
            nanosleep(&ts, NULL); /* let the client read GOAWAY before FIN */
            goto out;
        }
    }
out:
    close(fd);
    return NULL;
}

static uint16_t start_goaway_fake(goaway_fake *fake) {
    memset(fake, 0, sizeof(*fake));
    fake->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fake->listen_fd >= 0);
    int one = 1;
    setsockopt(fake->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(fake->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(fake->listen_fd, 4) == 0);
    socklen_t alen = sizeof(addr);
    assert(getsockname(fake->listen_fd, (struct sockaddr *)&addr, &alen) == 0);
    assert(pthread_create(&fake->tid, NULL, goaway_fake_thread, fake) == 0);
    return ntohs(addr.sin_port);
}

static void stop_goaway_fake(goaway_fake *fake) {
    shutdown(fake->listen_fd, SHUT_RDWR);
    close(fake->listen_fd);
    pthread_join(fake->tid, NULL);
}

/* --- client helpers --- */

static uint64_t test_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint8_t *build_request(const char *text, size_t *out_len) {
    cwist_pb_writer w;
    cwist_pb_writer_init(&w);
    assert(cwist_pb_write_string_field(&w, 1, text) == 0);
    uint8_t *copy = cwist_alloc(w.len);
    assert(copy != NULL);
    memcpy(copy, w.data, w.len);
    *out_len = w.len;
    cwist_pb_writer_free(&w);
    return copy;
}

/* Unary Who/Flaky-style call; decodes the "X:N" payload into buf. */
static cwist_grpc_status_t channel_who(cwist_grpc_channel *ch, const char *method,
                                       char *buf, size_t buf_cap, uint32_t *attempts) {
    size_t req_len;
    uint8_t *req = build_request("hi", &req_len);
    cwist_grpc_channel_call *cc =
        cwist_grpc_channel_call_start(ch, method, req, req_len, 0);
    assert(cc != NULL);
    if (attempts) *attempts = cwist_grpc_channel_call_attempts(cc);
    cwist_grpc_message msg;
    buf[0] = '\0';
    if (cwist_grpc_channel_call_recv(cc, &msg) == 1) {
        cwist_pb_reader r;
        cwist_pb_reader_init(&r, msg.data, msg.len);
        cwist_pb_field field;
        assert(cwist_pb_read_field(&r, &field) > 0);
        assert(field.number == 1 && field.wire_type == CWIST_PB_LEN);
        assert(field.len < buf_cap);
        memcpy(buf, field.bytes, field.len);
        buf[field.len] = '\0';
    }
    cwist_grpc_status_t status = cwist_grpc_channel_call_finish(cc, NULL);
    cwist_grpc_channel_call_destroy(cc);
    cwist_free(req);
    return status;
}

static const cwist_grpc_retry_policy RETRY_UNAVAIL = {
    .max_attempts = 3,
    .initial_backoff_ms = 10,
    .max_backoff_ms = 50,
    .backoff_multiplier = 2.0,
    .retryable_status_mask = CWIST_GRPC_STATUS_BIT(CWIST_GRPC_UNAVAILABLE),
};

/* --- tests --- */

static void test_pick_first_basic(uint16_t port_a, uint16_t port_b) {
    printf("  pick_first: sticky backend, channel state...\n");
    char target[128];
    snprintf(target, sizeof(target), "ipv4:127.0.0.1:%u,127.0.0.1:%u",
             (unsigned)port_a, (unsigned)port_b);
    cwist_grpc_channel *ch = cwist_grpc_channel_connect(target, NULL);
    assert(ch != NULL);
    assert(cwist_grpc_channel_get_state(ch) == CWIST_GRPC_CHANNEL_IDLE);

    char buf[64];
    for (int i = 0; i < 3; i++) {
        assert(channel_who(ch, "/cwist.test.Bk/Who", buf, sizeof(buf), NULL) ==
               CWIST_GRPC_OK);
        assert(strcmp(buf, "A:0") == 0); /* sticky on the first address */
    }
    assert(cwist_grpc_channel_get_state(ch) == CWIST_GRPC_CHANNEL_READY);
    cwist_grpc_channel_close(ch);
}

static void test_pick_first_failover(uint16_t port_b) {
    printf("  pick_first: failover to the next address...\n");
    uint16_t dead = closed_port();
    char target[128];
    snprintf(target, sizeof(target), "ipv4:127.0.0.1:%u,127.0.0.1:%u",
             (unsigned)dead, (unsigned)port_b);
    cwist_grpc_channel *ch = cwist_grpc_channel_connect(target, NULL);
    assert(ch != NULL);

    char buf[64];
    assert(channel_who(ch, "/cwist.test.Bk/Who", buf, sizeof(buf), NULL) ==
           CWIST_GRPC_OK);
    assert(strcmp(buf, "B:0") == 0);
    /* and it stays on B */
    assert(channel_who(ch, "/cwist.test.Bk/Who", buf, sizeof(buf), NULL) ==
           CWIST_GRPC_OK);
    assert(strcmp(buf, "B:0") == 0);
    cwist_grpc_channel_close(ch);
}

static void test_round_robin(uint16_t port_a, uint16_t port_b) {
    printf("  round_robin: successive calls rotate over READY backends...\n");
    char target[128];
    snprintf(target, sizeof(target), "ipv4:127.0.0.1:%u,127.0.0.1:%u",
             (unsigned)port_a, (unsigned)port_b);
    cwist_grpc_channel_options opts = { 0 };
    opts.lb_policy = CWIST_GRPC_LB_ROUND_ROBIN;
    cwist_grpc_channel *ch = cwist_grpc_channel_connect(target, &opts);
    assert(ch != NULL);
    assert(cwist_grpc_channel_get_state(ch) == CWIST_GRPC_CHANNEL_READY);

    const char *expect[] = { "A:0", "B:0", "A:0", "B:0", "A:0", "B:0" };
    for (int i = 0; i < 6; i++) {
        char buf[64];
        assert(channel_who(ch, "/cwist.test.Bk/Who", buf, sizeof(buf), NULL) ==
               CWIST_GRPC_OK);
        assert(strcmp(buf, expect[i]) == 0);
    }
    cwist_grpc_channel_close(ch);
}

static void test_retry_on_unavailable(test_backend *a, uint16_t port_a) {
    printf("  retry: UNAVAILABLE trailers-only is retried to success...\n");
    atomic_store(&a->state.flaky_failures, 2);
    int before = atomic_load(&a->state.calls);
    char target[96];
    snprintf(target, sizeof(target), "ipv4:127.0.0.1:%u", (unsigned)port_a);
    cwist_grpc_channel_options opts = { 0 };
    opts.retry_policy = &RETRY_UNAVAIL;
    cwist_grpc_channel *ch = cwist_grpc_channel_connect(target, &opts);
    assert(ch != NULL);

    char buf[64];
    uint32_t attempts = 0;
    assert(channel_who(ch, "/cwist.test.Bk/Flaky", buf, sizeof(buf), &attempts) ==
           CWIST_GRPC_OK);
    assert(attempts == 3);
    /* the winning attempt carried grpc-previous-rpc-attempts = 2 */
    assert(strcmp(buf, "A:2") == 0);
    assert(atomic_load(&a->state.calls) - before == 3);
    cwist_grpc_channel_close(ch);
}

static void test_non_retryable(uint16_t port_a) {
    printf("  retry: status outside retryableStatusCodes is not retried...\n");
    char target[96];
    snprintf(target, sizeof(target), "ipv4:127.0.0.1:%u", (unsigned)port_a);
    cwist_grpc_channel_options opts = { 0 };
    opts.retry_policy = &RETRY_UNAVAIL;
    cwist_grpc_channel *ch = cwist_grpc_channel_connect(target, &opts);
    assert(ch != NULL);

    char buf[64];
    uint32_t attempts = 0;
    assert(channel_who(ch, "/cwist.test.Bk/Fail", buf, sizeof(buf), &attempts) ==
           CWIST_GRPC_INVALID_ARGUMENT);
    assert(attempts == 1);
    cwist_grpc_channel_close(ch);
}

static void test_retry_exhaustion(test_backend *a, uint16_t port_a) {
    printf("  retry: maxAttempts bounds the attempts...\n");
    int before = atomic_load(&a->state.calls);
    char target[96];
    snprintf(target, sizeof(target), "ipv4:127.0.0.1:%u", (unsigned)port_a);
    cwist_grpc_channel_options opts = { 0 };
    opts.retry_policy = &RETRY_UNAVAIL; /* max_attempts = 3 */
    cwist_grpc_channel *ch = cwist_grpc_channel_connect(target, &opts);
    assert(ch != NULL);

    char buf[64];
    uint32_t attempts = 0;
    assert(channel_who(ch, "/cwist.test.Bk/Always", buf, sizeof(buf), &attempts) ==
           CWIST_GRPC_UNAVAILABLE);
    assert(attempts == 3);
    assert(atomic_load(&a->state.calls) - before == 3);
    cwist_grpc_channel_close(ch);
}

static void test_pushback_no_retry(test_backend *a, uint16_t port_a) {
    printf("  retry: negative grpc-retry-pushback-ms stops retries...\n");
    atomic_store(&a->state.pushback_failures, 5);
    a->state.pushback_ms = -1;
    char target[96];
    snprintf(target, sizeof(target), "ipv4:127.0.0.1:%u", (unsigned)port_a);
    cwist_grpc_channel_options opts = { 0 };
    opts.retry_policy = &RETRY_UNAVAIL;
    cwist_grpc_channel *ch = cwist_grpc_channel_connect(target, &opts);
    assert(ch != NULL);

    char buf[64];
    uint32_t attempts = 0;
    assert(channel_who(ch, "/cwist.test.Bk/Pushback", buf, sizeof(buf), &attempts) ==
           CWIST_GRPC_UNAVAILABLE);
    assert(attempts == 1); /* server asked not to retry (gRFC A6) */
    cwist_grpc_channel_close(ch);
}

static void test_pushback_delay(test_backend *a, uint16_t port_a) {
    printf("  retry: positive grpc-retry-pushback-ms sets the delay...\n");
    atomic_store(&a->state.pushback_failures, 1);
    a->state.pushback_ms = 100;
    char target[96];
    snprintf(target, sizeof(target), "ipv4:127.0.0.1:%u", (unsigned)port_a);
    cwist_grpc_channel_options opts = { 0 };
    opts.retry_policy = &RETRY_UNAVAIL;
    cwist_grpc_channel *ch = cwist_grpc_channel_connect(target, &opts);
    assert(ch != NULL);

    char buf[64];
    uint32_t attempts = 0;
    uint64_t start = test_now_ms();
    assert(channel_who(ch, "/cwist.test.Bk/Pushback", buf, sizeof(buf), &attempts) ==
           CWIST_GRPC_OK);
    uint64_t elapsed = test_now_ms() - start;
    assert(attempts == 2);
    assert(strcmp(buf, "A:1") == 0);
    assert(elapsed >= 50); /* exactly the pushback delay, no jitter (A6) */
    cwist_grpc_channel_close(ch);
}

static void test_throttle(test_backend *a, uint16_t port_a) {
    printf("  retry: throttling cancels retries under the threshold...\n");
    int before = atomic_load(&a->state.calls);
    char target[96];
    snprintf(target, sizeof(target), "ipv4:127.0.0.1:%u", (unsigned)port_a);
    static const cwist_grpc_retry_policy policy5 = {
        .max_attempts = 5,
        .initial_backoff_ms = 5,
        .max_backoff_ms = 20,
        .backoff_multiplier = 2.0,
        .retryable_status_mask = CWIST_GRPC_STATUS_BIT(CWIST_GRPC_UNAVAILABLE),
    };
    static const cwist_grpc_retry_throttle throttle = {
        .max_tokens = 3,
        .token_ratio = 0.1,
    };
    cwist_grpc_channel_options opts = { 0 };
    opts.retry_policy = &policy5;
    opts.retry_throttle = &throttle;
    cwist_grpc_channel *ch = cwist_grpc_channel_connect(target, &opts);
    assert(ch != NULL);

    char buf[64];
    uint32_t attempts = 0;
    assert(channel_who(ch, "/cwist.test.Bk/Always", buf, sizeof(buf), &attempts) ==
           CWIST_GRPC_UNAVAILABLE);
    /* tokens start at 3 (maxTokens): attempt 1 -> 2 > 1.5 retry allowed,
     * attempt 2 -> 1 <= 1.5 (maxTokens/2) throttled (gRFC A6). */
    assert(attempts == 2);
    assert(atomic_load(&a->state.calls) - before == 2);
    cwist_grpc_channel_close(ch);
}

static void test_deadline_across_attempts(uint16_t port_a) {
    printf("  retry: the call deadline spans every attempt...\n");
    char target[96];
    snprintf(target, sizeof(target), "ipv4:127.0.0.1:%u", (unsigned)port_a);
    static const cwist_grpc_retry_policy slow = {
        .max_attempts = 5,
        .initial_backoff_ms = 5000,
        .max_backoff_ms = 5000,
        .backoff_multiplier = 1.0,
        .retryable_status_mask = CWIST_GRPC_STATUS_BIT(CWIST_GRPC_UNAVAILABLE),
    };
    cwist_grpc_channel_options opts = { 0 };
    opts.retry_policy = &slow;
    cwist_grpc_channel *ch = cwist_grpc_channel_connect(target, &opts);
    assert(ch != NULL);

    size_t req_len;
    uint8_t *req = build_request("hi", &req_len);
    uint8_t *resp = NULL;
    size_t resp_len = 0;
    uint64_t start = test_now_ms();
    cwist_grpc_status_t status =
        cwist_grpc_channel_unary(ch, "/cwist.test.Bk/Always", req, req_len, 150,
                                 &resp, &resp_len, NULL);
    uint64_t elapsed = test_now_ms() - start;
    assert(status == CWIST_GRPC_DEADLINE_EXCEEDED);
    assert(resp == NULL);
    assert(elapsed < 2000); /* bounded by the 150ms deadline, not the 5s backoff */
    cwist_free(req);
    cwist_grpc_channel_close(ch);
}

static void test_json_service_config(test_backend *a, test_backend *b,
                                     uint16_t port_a, uint16_t port_b) {
    printf("  service config JSON: retryPolicy + loadBalancingConfig...\n");
    char target[128];
    snprintf(target, sizeof(target), "ipv4:127.0.0.1:%u,127.0.0.1:%u",
             (unsigned)port_a, (unsigned)port_b);
    cwist_grpc_channel *ch = cwist_grpc_channel_connect(target, NULL);
    assert(ch != NULL);

    static const char config[] =
        "{"
        "  \"loadBalancingConfig\": [{\"round_robin\": {}}],"
        "  \"retryThrottling\": {\"maxTokens\": 10, \"tokenRatio\": 0.1},"
        "  \"methodConfig\": [{"
        "    \"name\": [{\"service\": \"cwist.test.Bk\", \"method\": \"Flaky\"}],"
        "    \"retryPolicy\": {"
        "      \"maxAttempts\": 3,"
        "      \"initialBackoff\": \"0.010s\","
        "      \"maxBackoff\": \"0.050s\","
        "      \"backoffMultiplier\": 2,"
        "      \"retryableStatusCodes\": [\"UNAVAILABLE\"]"
        "    }"
        "  }]"
        "}";
    assert(cwist_grpc_channel_apply_service_config_json(ch, config) == 0);

    /* A fails once, B fails once: the retry must rotate A -> B -> A and
     * succeed on the third attempt. */
    atomic_store(&a->state.flaky_failures, 1);
    atomic_store(&b->state.flaky_failures, 1);
    char buf[64];
    uint32_t attempts = 0;
    assert(channel_who(ch, "/cwist.test.Bk/Flaky", buf, sizeof(buf), &attempts) ==
           CWIST_GRPC_OK);
    assert(attempts == 3);
    assert(strcmp(buf, "A:2") == 0);

    /* Methods without a matching methodConfig name get no retry policy. */
    atomic_store(&a->state.flaky_failures, 1);
    attempts = 0;
    assert(channel_who(ch, "/cwist.test.Bk/Always", buf, sizeof(buf), &attempts) ==
           CWIST_GRPC_UNAVAILABLE);
    assert(attempts == 1);

    /* A6 validation: maxAttempts must be an integer greater than 1. */
    static const char bad1[] =
        "{\"methodConfig\":[{\"name\":[{\"service\":\"s\"}],"
        "\"retryPolicy\":{\"maxAttempts\":1,\"initialBackoff\":\"0.1s\","
        "\"maxBackoff\":\"1s\",\"backoffMultiplier\":2,"
        "\"retryableStatusCodes\":[\"UNAVAILABLE\"]}}]}";
    assert(cwist_grpc_channel_apply_service_config_json(ch, bad1) == -1);
    /* Empty retryableStatusCodes is a validation error. */
    static const char bad2[] =
        "{\"methodConfig\":[{\"name\":[{\"service\":\"s\"}],"
        "\"retryPolicy\":{\"maxAttempts\":2,\"initialBackoff\":\"0.1s\","
        "\"maxBackoff\":\"1s\",\"backoffMultiplier\":2,"
        "\"retryableStatusCodes\":[]}}]}";
    assert(cwist_grpc_channel_apply_service_config_json(ch, bad2) == -1);
    /* Malformed duration. */
    static const char bad3[] =
        "{\"methodConfig\":[{\"name\":[{\"service\":\"s\"}],"
        "\"retryPolicy\":{\"maxAttempts\":2,\"initialBackoff\":\"100ms\","
        "\"maxBackoff\":\"1s\",\"backoffMultiplier\":2,"
        "\"retryableStatusCodes\":[\"UNAVAILABLE\"]}}]}";
    assert(cwist_grpc_channel_apply_service_config_json(ch, bad3) == -1);

    cwist_grpc_channel_close(ch);
}

static void test_transparent_retry_goaway(uint16_t port_b) {
    printf("  retry: GOAWAY last_stream_id < stream id retries transparently...\n");
    goaway_fake fake;
    uint16_t fake_port = start_goaway_fake(&fake);

    char target[128];
    snprintf(target, sizeof(target), "ipv4:127.0.0.1:%u,127.0.0.1:%u",
             (unsigned)fake_port, (unsigned)port_b);
    cwist_grpc_channel_options opts = { 0 };
    opts.retry_policy = &RETRY_UNAVAIL;
    cwist_grpc_channel *ch = cwist_grpc_channel_connect(target, &opts);
    assert(ch != NULL);

    char buf[64];
    uint32_t attempts = 0;
    assert(channel_who(ch, "/cwist.test.Bk/Who", buf, sizeof(buf), &attempts) ==
           CWIST_GRPC_OK);
    /* The fake refused the first wire attempt before server application
     * logic: one transparent retry (not counted against maxAttempts, gRFC
     * A6) lands on backend B, which reports grpc-previous-rpc-attempts = 1. */
    assert(atomic_load(&fake.served) == 1);
    assert(attempts == 1);
    assert(strcmp(buf, "B:1") == 0);

    stop_goaway_fake(&fake);
    cwist_grpc_channel_close(ch);
}

static void test_target_schemes(uint16_t port_a) {
    printf("  resolver: dns/ipv4/default-port and unsupported schemes...\n");
    char target[128];

    /* no scheme = dns */
    snprintf(target, sizeof(target), "localhost:%u", (unsigned)port_a);
    cwist_grpc_channel *ch = cwist_grpc_channel_connect(target, NULL);
    assert(ch != NULL);
    char buf[64];
    assert(channel_who(ch, "/cwist.test.Bk/Who", buf, sizeof(buf), NULL) ==
           CWIST_GRPC_OK);
    cwist_grpc_channel_close(ch);

    /* explicit dns:/// form */
    snprintf(target, sizeof(target), "dns:///localhost:%u", (unsigned)port_a);
    ch = cwist_grpc_channel_connect(target, NULL);
    assert(ch != NULL);
    cwist_grpc_channel_close(ch);

    /* unsupported transports fail cleanly */
    assert(cwist_grpc_channel_connect("unix:///tmp/cwist-test.sock", NULL) == NULL);
    assert(cwist_grpc_channel_connect("vsock:2:1234", NULL) == NULL);
    assert(cwist_grpc_channel_connect("ipv4:999.999.999.999:1", NULL) == NULL);
}

int main(void) {
    printf("Testing gRPC channel (LB + retry)...\n");

    static test_backend a, b;
    uint16_t port_a = start_backend(&a, 'A');
    uint16_t port_b = start_backend(&b, 'B');

    test_pick_first_basic(port_a, port_b);
    test_pick_first_failover(port_b);
    test_round_robin(port_a, port_b);
    test_retry_on_unavailable(&a, port_a);
    test_non_retryable(port_a);
    test_retry_exhaustion(&a, port_a);
    test_pushback_no_retry(&a, port_a);
    test_pushback_delay(&a, port_a);
    test_throttle(&a, port_a);
    test_deadline_across_attempts(port_a);
    test_json_service_config(&a, &b, port_a, port_b);
    test_transparent_retry_goaway(port_b);
    test_target_schemes(port_a);

    stop_backend(&a);
    stop_backend(&b);
    printf("test_grpc_channel: OK\n");
    return 0;
}
