/* Functional correctness for CWIST_POOL_SHARDS: every submitted task must
 * still run exactly once, from any shard count, with no double-free, no
 * lost task, and clean init/destroy. This does not test throughput (see
 * tests/bench_pool_contention.c for the manual lock-contention probe) -
 * it only guards the sharding refactor against correctness regressions in
 * the shared classic-pool code path (http_dynamic_worker_thread,
 * http_spawn_worker, cwist_http_pool_submit/destroy). */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cwist/net/http/http.h"

static _Atomic long g_completed = 0;

static void counting_handler(int fd, void *ctx) {
    (void)fd;
    (void)ctx;
    atomic_fetch_add_explicit(&g_completed, 1, memory_order_relaxed);
}

typedef struct {
    long ops;
} submitter_arg_t;

static void *submitter_thread(void *arg) {
    long ops = ((submitter_arg_t *)arg)->ops;
    for (long i = 0; i < ops; i++) {
        cwist_http_pool_submit(-1, counting_handler, NULL);
    }
    return NULL;
}

static void run_case(const char *shards_env, int nthreads, long ops_per_thread) {
    if (shards_env) {
        setenv("CWIST_POOL_SHARDS", shards_env, 1);
    } else {
        unsetenv("CWIST_POOL_SHARDS");
    }
    setenv("CWIST_C1M_MODE", "0", 1);
    setenv("CWIST_POOL_PREWARM", "16", 1);

    atomic_store_explicit(&g_completed, 0, memory_order_relaxed);
    long target = (long)nthreads * ops_per_thread;

    assert(cwist_http_pool_init() == 0);

    pthread_t *tids = calloc((size_t)nthreads, sizeof(pthread_t));
    submitter_arg_t sarg = { .ops = ops_per_thread };
    for (int i = 0; i < nthreads; i++) {
        assert(pthread_create(&tids[i], NULL, submitter_thread, &sarg) == 0);
    }
    for (int i = 0; i < nthreads; i++) {
        pthread_join(tids[i], NULL);
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 10;
    for (;;) {
        long done = atomic_load_explicit(&g_completed, memory_order_relaxed);
        if (done >= target) break;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        assert((now.tv_sec < deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec)) &&
               "timed out waiting for all tasks to complete");
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L };
        nanosleep(&ts, NULL);
    }

    /* No task ran twice, none were dropped. */
    assert(atomic_load_explicit(&g_completed, memory_order_relaxed) == target);

    cwist_http_pool_destroy();
    free(tids);

    printf("shards=%s threads=%d ops_per_thread=%ld total=%ld: PASS\n",
           shards_env ? shards_env : "1(default)", nthreads, ops_per_thread, target);
}

int main(void) {
    run_case(NULL, 8, 500);   /* default path unchanged */
    run_case("1", 8, 500);    /* explicit N=1 same as default */
    run_case("4", 16, 500);   /* sharded, thread count not a multiple of shard count */
    run_case("8", 24, 500);   /* sharded, more shards than half the submitters */
    run_case("64", 4, 200);   /* shard count exceeds submitter/worker count */
    return 0;
}
