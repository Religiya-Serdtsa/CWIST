/**
 * Regression for #25: a ready pipeline must not monopolize one reactor.
 * The first handler is gated while a second connection becomes ready. Check
 * service order, not a timing threshold, and drain the entire pipeline without
 * sending more bytes (buffered continuations must not wait for another read).
 */
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/shutdown.h>
#include <cwist/sys/io/reactor.h>
#include <cwist/net/http/async.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define REQUESTS 256
#define RESPONSE_CAP (256 * 1024)

static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int entered, released;
static int bulk_count, probe_at;
static long delay_us;
static cwist_reactor_t *reactor;
static int max_batch;
static bool destroy_pending;
static cwist_async *pending;

extern cwist_async_action_t cwist_app_http_handler_async(int, cwist_http_async_conn_t *);

static cwist_async_action_t dispatch(int fd, cwist_http_async_conn_t *conn) {
    if (!reactor) reactor = conn->reactor;
    int before = bulk_count;
    cwist_async_action_t action = cwist_app_http_handler_async(fd, conn);
    if (destroy_pending) {
        assert(action == CWIST_ASYNC_DEFER);
        pthread_mutex_lock(&gate_lock);
        entered = 1;
        pthread_cond_signal(&gate_cv);
        while (!released) pthread_cond_wait(&gate_cv, &gate_lock);
        pthread_mutex_unlock(&gate_lock);
    }
    if (bulk_count - before > max_batch) max_batch = bulk_count - before;
    return action;
}

static void wake_for_shutdown(void *unused) {
    (void)unused;
}

static void bulk_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    ++bulk_count;
    if (bulk_count == 1) {
        pthread_mutex_lock(&gate_lock);
        entered = 1;
        pthread_cond_signal(&gate_cv);
        while (!released) pthread_cond_wait(&gate_cv, &gate_lock);
        pthread_mutex_unlock(&gate_lock);
    }
    if (delay_us > 0) {
        struct timespec delay = {.tv_sec = delay_us / 1000000,
                                .tv_nsec = (delay_us % 1000000) * 1000};
        nanosleep(&delay, NULL);
    }
    char body[32];
    snprintf(body, sizeof(body), "item-%03d", bulk_count);
    cwist_sstring_assign(res->body, body);
}

static void defer_handler(cwist_http_request *req, cwist_http_response *res) {
    pending = cwist_async_defer(req, res);
    assert(pending);
    cwist_async_retain(pending);
}

static void probe_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    probe_at = bulk_count;
    cwist_sstring_assign(res->body, "probe");
}

static void write_all(int fd, const char *data, size_t len) {
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        data += n;
        len -= (size_t)n;
    }
}

/* Verify exact framing/body/order, not just a matching substring count. */
static void check_responses(char *data, size_t len) {
    size_t off = 0;
    for (int i = 1; i <= REQUESTS; ++i) {
        assert(off < len);
        assert(strncmp(data + off, "HTTP/1.1 200 ", 13) == 0);
        char *end = strstr(data + off, "\r\n\r\n");
        assert(end);
        char *cl = strstr(data + off, "Content-Length: ");
        assert(cl && cl < end);
        size_t body_len = (size_t)strtoul(cl + 16, NULL, 10);
        off = (size_t)(end - data) + 4;
        char body[32];
        snprintf(body, sizeof(body), "item-%03d", i);
        assert(body_len == strlen(body) && body_len <= len - off);
        assert(memcmp(data + off, body, body_len) == 0);
        off += body_len;
    }
    assert(off == len);
}

/* Queue a real deferred completion for the destructor's final post drain.
 * Keep the global run flag true: pool teardown alone must prohibit rearming. */
static void check_destroy_pending(cwist_app *app) {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    const char requests[] = "GET /defer HTTP/1.1\r\nHost: x\r\n\r\n"
                            "GET /bulk HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    write_all(fds[1], requests, sizeof(requests) - 1);
    assert(cwist_http_pool_submit_async(fds[0], dispatch, app));
    pthread_mutex_lock(&gate_lock);
    while (!entered) pthread_cond_wait(&gate_cv, &gate_lock);
    assert(cwist_async_respond(pending, CWIST_HTTP_OK, "text/plain", "done", 4));
    cwist_async_release(pending);
    /* Stop while the owner is gated, before it can drain the posted response. */
    cwist_reactor_stop(reactor);
    released = 1;
    pthread_cond_signal(&gate_cv);
    pthread_mutex_unlock(&gate_lock);
    cwist_http_pool_destroy();
    assert(atomic_load(&g_cwist_running));

    char response[4096] = {0};
    size_t used = 0;
    for (;;) {
        struct pollfd pfd = {.fd = fds[1], .events = POLLIN};
        assert(poll(&pfd, 1, 1000) == 1); /* Must receive EOF, not an orphan fd. */
        assert(used < sizeof(response) - 1);
        ssize_t n = read(fds[1], response + used, sizeof(response) - 1 - used);
        assert(n >= 0);
        if (n == 0) break;
        used += (size_t)n;
    }
    assert(strstr(response, "\r\n\r\ndone"));
    assert(bulk_count == 0);
    close(fds[1]);
    cwist_app_destroy(app);
    puts("Deferred completion during pool destruction test passed");
}

int main(void) {
    /* A hard watchdog catches lost continuations; no latency threshold decides
     * whether fairness passes. Optional delay is for the focused benchmark. */
    alarm(30);
    signal(SIGPIPE, SIG_IGN);
    setenv("CWIST_WORKERS", "1", 1);
    setenv("CWIST_WORKER_THREADS", "1", 1);
    setenv("CWIST_C1M_MODE", "1", 1);
    const char *delay = getenv("CWIST_TEST_HANDLER_DELAY_US");
    delay_us = delay ? strtol(delay, NULL, 10) : 0;
    bool half_close = getenv("CWIST_TEST_HALF_CLOSE") != NULL;
    bool truncated = getenv("CWIST_TEST_TRUNCATED") != NULL;
    destroy_pending = getenv("CWIST_TEST_DESTROY_PENDING") != NULL;
    assert(delay_us >= 0 && delay_us <= 10000);
    atomic_store(&g_cwist_running, true);

    cwist_app *app = cwist_app_create();
    assert(app);
    cwist_app_get(app, "/bulk", bulk_handler);
    cwist_app_get(app, "/probe", probe_handler);
    cwist_app_get(app, "/defer", defer_handler);
    assert(cwist_http_pool_init() == 0);
    if (destroy_pending) {
        check_destroy_pending(app);
        return 0;
    }

    int bulk[2], probe[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, bulk) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, probe) == 0);
    /* Fit all requests in the initial recv stash so the test cannot depend on
     * more input arriving to resume buffered work. */
    char pipeline[16384];
    size_t size = 0;
    for (int i = 1; i <= REQUESTS; ++i) {
        int n = snprintf(pipeline + size, sizeof(pipeline) - size,
                         "GET /bulk HTTP/1.1\r\nHost: x\r\n%s\r\n",
                         i == REQUESTS && !half_close ? "Connection: close\r\n" : "");
        assert(n > 0 && (size_t)n < sizeof(pipeline) - size);
        size += (size_t)n;
    }
    write_all(bulk[1], pipeline, size);
    if (half_close) {
        if (truncated) {
            const char partial[] = "GET /bulk HTTP/1.1\r\nHost: ";
            write_all(bulk[1], partial, sizeof(partial) - 1);
        }
        assert(shutdown(bulk[1], SHUT_WR) == 0);
    }
    assert(cwist_http_pool_submit_async(bulk[0], dispatch, app));
    pthread_mutex_lock(&gate_lock);
    while (!entered) pthread_cond_wait(&gate_cv, &gate_lock);
    const char request[] = "GET /probe HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    write_all(probe[1], request, sizeof(request) - 1);
    assert(cwist_http_pool_submit_async(probe[0], dispatch, app));
    struct timespec start, finish;
    clock_gettime(CLOCK_MONOTONIC, &start);
    released = 1;
    pthread_cond_signal(&gate_cv);
    pthread_mutex_unlock(&gate_lock);

    struct pollfd fds[2] = {{.fd = bulk[1], .events = POLLIN},
                           {.fd = probe[1], .events = POLLIN}};
    char *responses[2] = {calloc(1, RESPONSE_CAP), calloc(1, RESPONSE_CAP)};
    assert(responses[0] && responses[1]);
    size_t used[2] = {0, 0};
    int live = 2;
    double probe_ms = -1;
    while (live) {
        int n = poll(fds, 2, 10000);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !fds[i].revents) continue;
            assert(used[i] < RESPONSE_CAP - 1);
            ssize_t got = read(fds[i].fd, responses[i] + used[i], RESPONSE_CAP - 1 - used[i]);
            if (got < 0 && errno == EINTR) continue;
            assert(got >= 0);
            if (got == 0) {
                close(fds[i].fd);
                fds[i].fd = -1;
                --live;
                if (i == 1) {
                    clock_gettime(CLOCK_MONOTONIC, &finish);
                    probe_ms = (finish.tv_sec - start.tv_sec) * 1000.0 +
                               (finish.tv_nsec - start.tv_nsec) / 1e6;
                }
            } else {
                used[i] += (size_t)got;
            }
        }
    }
    atomic_store(&g_cwist_running, false);
    /* Wake kqueue too: its idle wait does not have the Linux timeout. */
    cwist_reactor_post_t wake = { .cb = wake_for_shutdown };
    assert(cwist_reactor_post(reactor, &wake));
    cwist_http_pool_destroy();
    check_responses(responses[0], used[0]);
    assert(strstr(responses[1], "\r\n\r\nprobe"));
    assert(bulk_count == REQUESTS);
    printf("pipeline=%d probe_after=%d probe_ms=%.3f delay_us=%ld\n",
           bulk_count, probe_at, probe_ms, delay_us);
    fflush(stdout);
    free(responses[0]);
    free(responses[1]);
    cwist_app_destroy(app);
    assert(probe_at > 0 && probe_at < REQUESTS);
    assert(max_batch <= 16);
    puts("HTTP reactor fairness test passed");
    return 0;
}
