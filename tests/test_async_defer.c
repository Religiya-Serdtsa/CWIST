/**
 * @file test_async_defer.c
 * @brief Deferred-response (async handler) API tests.
 *
 * Forks a child running the server (mode selected by CWIST_C1M_MODE, exactly
 * like the production default) and drives it with raw socket clients.
 * Covers: basic defer via a 1-thread scheduler, reactor non-blocking during
 * a defer, keep-alive after a deferred response, timeout -> 504, one-shot
 * respond race, and pipelined ordering across a deferred first request.
 */

#include <cwist/sys/app/app.h>
#include <cwist/sys/app/shutdown.h>
#include <cwist/net/http/async.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define TEST_PORT 19997
#define TEST_HOST "127.0.0.1"

/* --- Server side --------------------------------------------------------- */

static void respond_later_job(void *arg) {
    cwist_async *a = (cwist_async *)arg;
    cwist_async_respond(a, CWIST_HTTP_OK, "text/plain", "deferred-body", 13);
}

static void schedule_respond(cwist_http_request *req, cwist_async *a, uint64_t delay_ms) {
    cwist_scheduler_t *s = cwist_app_get_scheduler(req->app);
    if (!s || !cwist_scheduler_schedule(s, respond_later_job, a, delay_ms)) {
        cwist_async_abort(a, CWIST_HTTP_INTERNAL_ERROR);
    }
}

static void defer200_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_async *a = cwist_async_defer(req, res);
    if (!a) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }
    schedule_respond(req, a, 150);
}

static void slow250_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_async *a = cwist_async_defer(req, res);
    if (!a) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }
    schedule_respond(req, a, 250);
}

static void timeout_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_async *a = cwist_async_defer(req, res);
    if (!a) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }
    cwist_async_set_timeout(a, 50);
    /* Never respond: the timeout must route a 504. */
}

static _Atomic int g_race_wins = 0;

static void *race_thread(void *arg) {
    cwist_async *a = (cwist_async *)arg;
    if (cwist_async_respond(a, CWIST_HTTP_OK, "text/plain", "race-winner", 11)) {
        atomic_fetch_add(&g_race_wins, 1);
    }
    return NULL;
}

static void race_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_async *a = cwist_async_defer(req, res);
    if (!a) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }
    pthread_t t1, t2;
    pthread_create(&t1, NULL, race_thread, a);
    pthread_create(&t2, NULL, race_thread, a);
    pthread_detach(t1);
    pthread_detach(t2);
}

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "second-ok");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    res->status_code = CWIST_HTTP_OK;
}

/* Large deferred body: big enough that a client which never reads forces the
 * completion into the parked POLLOUT-resumable writer on the reactor path. */
#define BIG_BODY_LEN (4 * 1024 * 1024)
static char *g_big_body;

static char big_body_byte(size_t i) {
    return (char)('a' + (i % 26));
}

static void respond_big_job(void *arg) {
    cwist_async *a = (cwist_async *)arg;
    cwist_async_respond(a, CWIST_HTTP_OK, "application/octet-stream", g_big_body, BIG_BODY_LEN);
}

static void bigdefer_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_async *a = cwist_async_defer(req, res);
    if (!a) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }
    cwist_scheduler_t *s = cwist_app_get_scheduler(req->app);
    if (!s || !cwist_scheduler_schedule(s, respond_big_job, a, 50)) {
        cwist_async_abort(a, CWIST_HTTP_INTERNAL_ERROR);
    }
}

/* --- Client side --------------------------------------------------------- */

static int connect_to_server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TEST_PORT),
    };
    inet_pton(AF_INET, TEST_HOST, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const char *req) {
    size_t len = strlen(req), sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, req + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Read exactly one HTTP/1.x response (headers + Content-Length body) into
 * buf; extra pipelined bytes are preserved in *pending for the next call. */
struct client_conn {
    int fd;
    char pending[8192];
    size_t pending_len;
};

static int read_one_response(struct client_conn *c, char *buf, size_t buf_size) {
    size_t total = 0;
    size_t header_end = 0, content_length = 0;
    bool parsed = false;
    buf[0] = '\0';
    for (;;) {
        if (!parsed && c->pending_len > 0) {
            size_t copy = c->pending_len < buf_size - 1 - total ? c->pending_len : buf_size - 1 - total;
            memcpy(buf + total, c->pending, copy);
            total += copy;
            memmove(c->pending, c->pending + copy, c->pending_len - copy);
            c->pending_len -= copy;
            buf[total] = '\0';
        }
        if (!parsed) {
            char *he = strstr(buf, "\r\n\r\n");
            if (he) {
                header_end = (size_t)(he - buf) + 4;
                char *cl = strcasestr(buf, "Content-Length:");
                if (!cl || (size_t)(cl - buf) > header_end) return -1;
                content_length = (size_t)atoi(cl + 15);
                parsed = true;
            }
        }
        if (parsed && total >= header_end + content_length) break;
        if (total >= buf_size - 1) return -1;
        ssize_t n = recv(c->fd, buf + total, buf_size - 1 - total, 0);
        if (n <= 0) return -1;
        total += (size_t)n;
        buf[total] = '\0';
    }
    /* Stash any bytes past this response for the next read. */
    size_t used = header_end + content_length;
    if (total > used) {
        memcpy(c->pending, buf + used, total - used);
        c->pending_len = total - used;
        buf[used] = '\0';
    }
    return (int)used;
}

static bool has_code(const char *buf, const char *code) {
    char prefix[24];
    snprintf(prefix, sizeof(prefix), "HTTP/1.1 %s", code);
    return strstr(buf, prefix) != NULL;
}

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void) {
    printf("Testing deferred responses (async handlers)...\n");

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* The app scheduler's threads do not survive the per-worker fork, so
         * keep the server single-process for the deferred-response tests. */
        setenv("CWIST_WORKERS", "1", 1);
        cwist_app *app = cwist_app_create();
        if (!app) _exit(1);
        cwist_app_use_scheduler(app, 1, 16);
        cwist_app_get(app, "/defer200", defer200_handler);
        cwist_app_get(app, "/slow250", slow250_handler);
        cwist_app_get(app, "/timeout", timeout_handler);
        cwist_app_get(app, "/race", race_handler);
        cwist_app_get(app, "/hello", hello_handler);
        cwist_app_get(app, "/bigdefer", bigdefer_handler);
        g_big_body = malloc(BIG_BODY_LEN);
        if (!g_big_body) _exit(1);
        for (size_t i = 0; i < BIG_BODY_LEN; i++) g_big_body[i] = big_body_byte(i);
        g_cwist_drain_timeout_sec = 1;
        int rc = cwist_app_listen(app, TEST_PORT);
        int wins = atomic_load(&g_race_wins);
        cwist_app_destroy(app);
        _exit(rc == 0 && wins <= 1 ? 0 : 1);
    }

    usleep(400000);
    int failures = 0;
    char buf[8192];

    /* 1. Basic deferred response. */
    {
        struct client_conn c = { .fd = connect_to_server(), .pending_len = 0 };
        CHECK(c.fd >= 0, "connect for basic defer");
        if (c.fd >= 0) {
            send_all(c.fd, "GET /defer200 HTTP/1.1\r\nHost: localhost\r\n\r\n");
            int n = read_one_response(&c, buf, sizeof(buf));
            CHECK(n > 0 && has_code(buf, "200"), "basic defer returns 200");
            CHECK(strstr(buf, "deferred-body") != NULL, "basic defer body");
            close(c.fd);
        }
    }

    /* 2. Reactor/pool not blocked while a request is deferred. */
    {
        struct client_conn slow = { .fd = connect_to_server(), .pending_len = 0 };
        CHECK(slow.fd >= 0, "connect for concurrency (slow)");
        struct client_conn fast = { .fd = connect_to_server(), .pending_len = 0 };
        CHECK(fast.fd >= 0, "connect for concurrency (fast)");
        if (slow.fd >= 0 && fast.fd >= 0) {
            send_all(slow.fd, "GET /slow250 HTTP/1.1\r\nHost: localhost\r\n\r\n");
            send_all(fast.fd, "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n");
            int n = read_one_response(&fast, buf, sizeof(buf));
            CHECK(n > 0 && has_code(buf, "200") && strstr(buf, "second-ok"),
                  "second connection served during defer");
            n = read_one_response(&slow, buf, sizeof(buf));
            CHECK(n > 0 && has_code(buf, "200") && strstr(buf, "deferred-body"),
                  "slow deferred response completes");
        }
        if (slow.fd >= 0) close(slow.fd);
        if (fast.fd >= 0) close(fast.fd);
    }

    /* 3. Keep-alive: second request on the same connection after a defer. */
    {
        struct client_conn c = { .fd = connect_to_server(), .pending_len = 0 };
        CHECK(c.fd >= 0, "connect for keep-alive");
        if (c.fd >= 0) {
            send_all(c.fd, "GET /defer200 HTTP/1.1\r\nHost: localhost\r\n\r\n");
            int n = read_one_response(&c, buf, sizeof(buf));
            CHECK(n > 0 && has_code(buf, "200"), "keep-alive deferred response");
            send_all(c.fd, "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n");
            n = read_one_response(&c, buf, sizeof(buf));
            CHECK(n > 0 && has_code(buf, "200") && strstr(buf, "second-ok"),
                  "second request on kept-alive connection");
            close(c.fd);
        }
    }

    /* 4. Timeout: never-responded defer answers 504. */
    {
        struct client_conn c = { .fd = connect_to_server(), .pending_len = 0 };
        CHECK(c.fd >= 0, "connect for timeout");
        if (c.fd >= 0) {
            send_all(c.fd, "GET /timeout HTTP/1.1\r\nHost: localhost\r\n\r\n");
            int n = read_one_response(&c, buf, sizeof(buf));
            CHECK(n > 0 && has_code(buf, "504"), "timeout returns 504");
            close(c.fd);
        }
    }

    /* 5. One-shot race: two threads respond; exactly one response arrives. */
    {
        struct client_conn c = { .fd = connect_to_server(), .pending_len = 0 };
        CHECK(c.fd >= 0, "connect for race");
        if (c.fd >= 0) {
            send_all(c.fd, "GET /race HTTP/1.1\r\nHost: localhost\r\n\r\n");
            int n = read_one_response(&c, buf, sizeof(buf));
            CHECK(n > 0 && has_code(buf, "200") && strstr(buf, "race-winner"),
                  "race produces exactly one response");
            close(c.fd);
        }
    }

    /* 6. Pipeline ordering: first request defers, second must not jump ahead.
     * C1M only: the classic pool handler keeps its recv stash on the stack of
     * the returned frame, so pipelined bytes past a deferred request cannot be
     * preserved there. */
    const char *c1m_env = getenv("CWIST_C1M_MODE");
    bool c1m = !(c1m_env && (c1m_env[0] == '0' || strcmp(c1m_env, "false") == 0));
    if (c1m) {
        struct client_conn c = { .fd = connect_to_server(), .pending_len = 0 };
        CHECK(c.fd >= 0, "connect for pipelining");
        if (c.fd >= 0) {
            send_all(c.fd,
                     "GET /defer200 HTTP/1.1\r\nHost: localhost\r\n\r\n"
                     "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
            int n = read_one_response(&c, buf, sizeof(buf));
            CHECK(n > 0 && strstr(buf, "deferred-body") != NULL,
                  "pipelined first response is the deferred one");
            n = read_one_response(&c, buf, sizeof(buf));
            CHECK(n > 0 && strstr(buf, "second-ok") != NULL,
                  "pipelined second response in order");
            close(c.fd);
        }
    }

    /* 7. Slow client: a large deferred response cannot drain immediately, so
     * the completion parks the remainder on POLLOUT; once the client starts
     * reading, the full body must arrive and keep-alive must still work. */
    {
        int fd = connect_to_server();
        CHECK(fd >= 0, "connect for slow-client park");
        if (fd >= 0) {
            int rcvbuf = 4096;
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
            send_all(fd, "GET /bigdefer HTTP/1.1\r\nHost: localhost\r\n\r\n");
            /* Do not read: the server's send buffer fills and the deferred
             * completion must park instead of blocking a worker/reactor. */
            usleep(500000);

            char hdr[4096];
            size_t hlen = 0;
            size_t header_end = 0;
            while (!header_end && hlen < sizeof(hdr) - 1) {
                ssize_t n = recv(fd, hdr + hlen, sizeof(hdr) - 1 - hlen, 0);
                if (n <= 0) break;
                hlen += (size_t)n;
                hdr[hlen] = '\0';
                char *he = strstr(hdr, "\r\n\r\n");
                if (he) header_end = (size_t)(he - hdr) + 4;
            }
            CHECK(header_end > 0 && has_code(hdr, "200"), "parked response headers arrive");
            size_t content_length = 0;
            if (header_end > 0) {
                char *cl = strcasestr(hdr, "Content-Length:");
                if (cl && (size_t)(cl - hdr) < header_end) content_length = (size_t)atoi(cl + 15);
            }
            CHECK(content_length == BIG_BODY_LEN, "parked response Content-Length");

            /* Bytes past the header end are already body. */
            uint64_t sum = 0;
            size_t got = 0;
            for (size_t i = header_end; i < hlen; i++) sum += (unsigned char)hdr[i];
            got = hlen - header_end;
            char chunk[16384];
            while (got < content_length) {
                ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
                if (n <= 0) break;
                for (ssize_t i = 0; i < n; i++) sum += (unsigned char)chunk[i];
                got += (size_t)n;
            }
            uint64_t expected = 0;
            for (size_t i = 0; i < BIG_BODY_LEN; i++) expected += (unsigned char)big_body_byte(i);
            CHECK(got == (size_t)BIG_BODY_LEN && sum == expected,
                  "parked writer delivers the full body intact");

            /* Keep-alive rearm after the parked writer drains. */
            struct client_conn c = { .fd = fd, .pending_len = 0 };
            send_all(fd, "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n");
            int n = read_one_response(&c, buf, sizeof(buf));
            CHECK(n > 0 && has_code(buf, "200") && strstr(buf, "second-ok"),
                  "connection reusable after parked write drains");
            close(fd);
        }
    }

    kill(pid, SIGTERM);
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: server child exited abnormally (status=%d)\n", status);
        failures++;
    }

    if (failures == 0) {
        printf("All async defer tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d async defer test(s) failed.\n", failures);
    return 1;
}
