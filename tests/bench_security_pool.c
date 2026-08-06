#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/waf.h>
#include <cwist/core/db/pool.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define WAF_ITERATIONS 1000000UL
#define POOL_THREADS 8
#define POOL_ITERATIONS 50000UL

typedef struct { cwist_db_pool_t *pool; } pool_worker;

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void *pool_lease_loop(void *opaque) {
    pool_worker *worker = opaque;
    for (unsigned long i = 0; i < POOL_ITERATIONS; ++i) {
        cwist_db *db = cwist_db_pool_acquire(worker->pool);
        assert(db != NULL);
        cwist_db_pool_release(worker->pool, db);
    }
    return NULL;
}

int main(void) {
    const char *safe = "name=Jane+Doe&note=the+quick+brown+fox+jumps+over+the+lazy+dog";
    uint64_t start = monotonic_ns();
    for (unsigned long i = 0; i < WAF_ITERATIONS; ++i) assert(cwist_waf_is_safe(safe, strlen(safe)));
    uint64_t waf_elapsed = monotonic_ns() - start;

    cwist_db_pool_t *pool = cwist_db_pool_create(":memory:", 8);
    assert(pool != NULL);
    pthread_t threads[POOL_THREADS];
    pool_worker worker = { .pool = pool };
    start = monotonic_ns();
    for (size_t i = 0; i < POOL_THREADS; ++i) assert(pthread_create(&threads[i], NULL, pool_lease_loop, &worker) == 0);
    for (size_t i = 0; i < POOL_THREADS; ++i) assert(pthread_join(threads[i], NULL) == 0);
    uint64_t pool_elapsed = monotonic_ns() - start;
    cwist_db_pool_destroy(pool);

    printf("waf: %.2f M checks/s (%lu checks)\n", (double)WAF_ITERATIONS * 1000.0 / (double)waf_elapsed, WAF_ITERATIONS);
    printf("pool: %.2f M acquire-release/s (%lu leases, %d threads)\n", (double)(POOL_THREADS * POOL_ITERATIONS) * 1000.0 / (double)pool_elapsed, (unsigned long)(POOL_THREADS * POOL_ITERATIONS), POOL_THREADS);
    return 0;
}
