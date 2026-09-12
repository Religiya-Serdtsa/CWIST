/* Ad hoc throughput probe for the classic pool's submitter-side lock.
 * Not part of the regression suite (no `make check` wiring) - built and
 * run manually to decide whether CWIST_POOL_SHARDS is worth carrying as a
 * real feature. Spins SUBMITTER_THREADS threads hammering
 * cwist_http_pool_submit() with a handler that does a tiny amount of work
 * and immediately returns, so the bottleneck under test is pool-side lock
 * contention, not handler work. Prints submissions/sec and wall time.
 *
 * Usage: CWIST_C1M_MODE=0 CWIST_POOL_SHARDS=<n> CWIST_POOL_PREWARM=<n> \
 *        ./bench_pool_contention <submitter_threads> <ops_per_thread>
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cwist/net/http/http.h"

static _Atomic long g_completed = 0;
static long g_target = 0;

static void dummy_handler(int fd, void *ctx) {
    (void)fd;
    (void)ctx;
    /* A handful of arithmetic ops so the queue/lock path dominates without
     * a syscall (no fd is opened - fd is always -1 in this harness). */
    volatile long x = 0;
    for (int i = 0; i < 50; i++) x += i;
    atomic_fetch_add_explicit(&g_completed, 1, memory_order_relaxed);
}

typedef struct {
    long ops;
} submitter_arg_t;

static void *submitter_thread(void *arg) {
    long ops = ((submitter_arg_t *)arg)->ops;
    for (long i = 0; i < ops; i++) {
        cwist_http_pool_submit(-1, dummy_handler, NULL);
    }
    return NULL;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    int nthreads = argc > 1 ? atoi(argv[1]) : 16;
    long ops = argc > 2 ? atol(argv[2]) : 20000;
    g_target = (long)nthreads * ops;

    if (cwist_http_pool_init() != 0) {
        fprintf(stderr, "pool init failed\n");
        return 1;
    }

    pthread_t *tids = calloc((size_t)nthreads, sizeof(pthread_t));
    submitter_arg_t sarg = { .ops = ops };

    double t0 = now_sec();
    for (int i = 0; i < nthreads; i++) {
        pthread_create(&tids[i], NULL, submitter_thread, &sarg);
    }
    for (int i = 0; i < nthreads; i++) {
        pthread_join(tids[i], NULL);
    }
    /* Drain: wait for workers to finish processing the queued tasks. */
    while (atomic_load_explicit(&g_completed, memory_order_relaxed) < g_target) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L };
        nanosleep(&ts, NULL);
    }
    double t1 = now_sec();

    double elapsed = t1 - t0;
    printf("threads=%d ops_per_thread=%ld total=%ld elapsed=%.4fs throughput=%.0f ops/s\n",
           nthreads, ops, g_target, elapsed, (double)g_target / elapsed);

    cwist_http_pool_destroy();
    free(tids);
    return 0;
}
