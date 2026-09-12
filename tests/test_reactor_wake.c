#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Standalone reactor/kernel contract test. Include the implementation to
 * reject io_uring fallback and inspect completion readiness, without adding a
 * public test API. Only the allocator and process shutdown flag are replaced;
 * all polling, eventfd, posting, and run-loop code is production code.
 *
 * --bench prints post-to-callback samples; timings are diagnostic, not gates.
 * CWIST_TEST_REQUIRE_IO_URING=1 fails rather than skipping a denied backend.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../src/sys/io/reactor.c"

void *cwist_alloc(size_t size) { return calloc(1, size); }
void cwist_free(void *ptr) { free(ptr); }
atomic_int g_cwist_running = 1;

enum { ROUNDS = 64, BATCH = 128 };
static cwist_reactor_t *loop;
static pthread_t owner;
static cwist_reactor_post_t chain[ROUNDS], finish;
static struct timespec sent[ROUNDS];
static double elapsed[ROUNDS];
static int delivered;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int requested = -1;
static bool quitting;

static double milliseconds(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
}

static void noop(void *ctx) { (void)ctx; }

static void request_next(int index) {
    assert(pthread_mutex_lock(&mu) == 0);
    requested = index;
    assert(pthread_cond_signal(&cv) == 0);
    assert(pthread_mutex_unlock(&mu) == 0);
}

static void delivered_cb(void *ctx) {
    int index = (int)(intptr_t)ctx;
    struct timespec now;
    assert(pthread_equal(pthread_self(), owner));
    assert(index == delivered++);
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    elapsed[index] = milliseconds(sent[index], now);
    if (index + 1 < ROUNDS) {
        request_next(index + 1);
    } else {
        cwist_reactor_stop(loop);
        /* stop() does not wake a wait after the top-of-loop post drain.
         * Queue a final no-op so this test is not about that separate API. */
        finish.cb = noop;
        assert(cwist_reactor_post(loop, &finish));
    }
}

static void *producer(void *unused) {
    (void)unused;
    for (;;) {
        assert(pthread_mutex_lock(&mu) == 0);
        while (requested < 0 && !quitting)
            assert(pthread_cond_wait(&cv, &mu) == 0);
        if (quitting) {
            assert(pthread_mutex_unlock(&mu) == 0);
            return NULL;
        }
        int index = requested;
        requested = -1;
        assert(pthread_mutex_unlock(&mu) == 0);
        chain[index].cb = delivered_cb;
        chain[index].ctx = (void *)(intptr_t)index;
        assert(clock_gettime(CLOCK_MONOTONIC, &sent[index]) == 0);
        assert(cwist_reactor_post(loop, &chain[index]));
    }
}

#ifdef __linux__
static void burst_cb(void *ctx) {
    assert(pthread_equal(pthread_self(), owner));
    assert((int)(intptr_t)ctx == delivered++);
    if (delivered == BATCH) cwist_reactor_stop(loop);
}

static void *burst_producer(void *arg) {
    cwist_reactor_post_t *nodes = arg;
    for (int i = 0; i < BATCH; i++) {
        nodes[i].cb = burst_cb;
        nodes[i].ctx = (void *)(intptr_t)i;
        assert(cwist_reactor_post(loop, &nodes[i]));
    }
    return NULL;
}
#endif

/* A readable post wake must produce a completion, not merely become visible
 * at the next shutdown polling timeout. No sockets can rescue the wake here.
 * Repeat on the same ring, using production dispatch to consume/re-arm it. */
static void check_uring_wake(void) {
#ifdef __linux__
    if (loop->impl.use_epoll) return;
    for (int epoch = 0; epoch < 4; epoch++) {
        cwist_reactor_post_t nodes[BATCH] = {0};
        pthread_t thread;
        delivered = 0;
        assert(pthread_create(&thread, NULL, burst_producer, nodes) == 0);
        assert(pthread_join(thread, NULL) == 0);
        const struct __kernel_timespec deadline = { .tv_sec = 1 };
        int ret;
        do {
            ret = sys_io_uring_enter_timeout(loop->impl.ring_fd,
                        loop->sq_unsubmitted, 1, IORING_ENTER_GETEVENTS, &deadline);
        } while (ret < 0 && errno == EINTR);
        if (ret < 0) {
            perror("posted wake did not generate a completion");
            exit(1);
        }
        loop->sq_unsubmitted = 0;
        /* A positive submission count can mask a wait timeout. Require a
         * real CQE too, including after each production dispatch/re-arm. */
        assert(__atomic_load_n(loop->impl.cq_tail, __ATOMIC_ACQUIRE) !=
               __atomic_load_n(loop->impl.cq_head, __ATOMIC_ACQUIRE));
        cwist_reactor_run(loop);
        assert(delivered == BATCH);
    }
    /* The drained input poll must be idle. Retaining output registration on
     * the same fd could otherwise feed completions back into themselves. */
    const struct __kernel_timespec idle = { .tv_nsec = 10000000 };
    (void)sys_io_uring_enter_timeout(loop->impl.ring_fd,
                    loop->sq_unsubmitted, 1, IORING_ENTER_GETEVENTS, &idle);
    loop->sq_unsubmitted = 0;
    assert(__atomic_load_n(loop->impl.cq_tail, __ATOMIC_ACQUIRE) ==
           __atomic_load_n(loop->impl.cq_head, __ATOMIC_ACQUIRE));
#endif
}

static cwist_reactor_t *create_checked(void) {
    cwist_reactor_t *r = cwist_reactor_create();
    assert(r != NULL);
#ifdef __linux__
    printf("backend=%s\n", r->impl.use_epoll ? "epoll" : "io_uring");
    if (r->impl.use_epoll && getenv("CWIST_TEST_REQUIRE_IO_URING")) {
        fprintf(stderr, "required io_uring backend unavailable\n");
        cwist_reactor_destroy(r);
        exit(1);
    }
#else
    puts("backend=kqueue");
#endif
    return r;
}

int main(int argc, char **argv) {
    bool bench = argc == 2 && strcmp(argv[1], "--bench") == 0;
    alarm(30); /* Watchdog only, not the correctness oracle. */
    owner = pthread_self();
    loop = create_checked();
    if (!bench) check_uring_wake();
    cwist_reactor_destroy(loop);
    loop = create_checked();
    delivered = 0;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, producer, NULL) == 0);
    request_next(0);
    cwist_reactor_run(loop);
    assert(pthread_mutex_lock(&mu) == 0);
    quitting = true;
    assert(pthread_cond_signal(&cv) == 0);
    assert(pthread_mutex_unlock(&mu) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(delivered == ROUNDS);
    cwist_reactor_destroy(loop);
    for (int i = 0; i < ROUNDS; i++) printf("post_ms[%d]=%.6f\n", i, elapsed[i]);
    puts("reactor wake, repeated re-arm, FIFO and owner-thread delivery: PASS");
    return 0;
}
