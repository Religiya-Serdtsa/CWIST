/* Manual throughput probe for CWIST_INTERCEPT_MALLOC (see
 * include/cwist/core/mem/intercept.h and docs/GC.md section 5). Not part
 * of `make test` - built and run on demand to answer one question: is the
 * "zero cost when full-GC is off" claim actually true, and how much does
 * a shimmed malloc/free cost when full-GC is on?
 *
 * Three configurations, same workload (alloc/write/free a small block in
 * a tight loop, single-threaded so the numbers are about per-call
 * overhead, not lock contention):
 *   1. baseline  - plain malloc/free, no shim at all (this file compiled
 *                  without CWIST_INTERCEPT_MALLOC in a second pass - see
 *                  the Makefile target, which builds it once with the
 *                  macro and once without via BASELINE).
 *   2. shim, full-GC off (default) - shim active, cwist_full_gc() never
 *                  called. Every call pays the shim's relaxed atomic
 *                  load (cwist_full_gc_enabled()) and one extra function
 *                  call versus calling libc directly, nothing else.
 *   3. shim, full-GC on - shim active, cwist_full_gc(true) called once at
 *                  startup. Every malloc registers with the pending-sweep
 *                  list and every free (of a still-tracked pointer)
 *                  removes it - this is the real cost of the safety net.
 *
 * Usage: ./bench_malloc_intercept <iterations>
 *        (run once as bench_malloc_intercept_baseline, once as
 *        bench_malloc_intercept - see the Makefile targets)
 */
#ifndef BASELINE
#define CWIST_INTERCEPT_MALLOC
#include <cwist/core/mem/intercept.h>
#include <cwist/core/mem/gc.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Small, mixed-size allocations mimicking a request/response handler's
 * typical scratch buffers - not one fixed size, so the allocator's
 * size-class handling isn't the only thing exercised. */
static size_t pick_size(long i) {
    static const size_t sizes[] = { 16, 64, 128, 256, 512 };
    return sizes[i % (long)(sizeof(sizes) / sizeof(sizes[0]))];
}

static double run_iterations(long n) {
    double t0 = now_sec();
    for (long i = 0; i < n; i++) {
        size_t sz = pick_size(i);
        char *p = malloc(sz);
        if (!p) { fprintf(stderr, "alloc failed at %ld\n", i); exit(1); }
        memset(p, (int)(i & 0xff), sz);
        /* Occasionally realloc, like a growing response buffer would. */
        if ((i & 7) == 0) {
            p = realloc(p, sz * 2);
            if (!p) { fprintf(stderr, "realloc failed at %ld\n", i); exit(1); }
        }
        free(p);
    }
    return now_sec() - t0;
}

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 2000000;

#ifndef BASELINE
    const char *mode = getenv("CWIST_FULL_GC_ON");
    const char *label = "shim, full-GC off";
    if (mode && mode[0] == '1') {
        cwist_full_gc(true);
        label = "shim, full-GC on";
    }
#else
    const char *label = "baseline (no shim)";
#endif

    /* Warmup: let the allocator settle into steady state before timing. */
    run_iterations(n / 10 < 10000 ? 10000 : n / 10);

    double elapsed = run_iterations(n);
    printf("%-20s iterations=%-9ld elapsed=%.4fs %.0f ops/s (%.1f ns/op)\n",
           label, n, elapsed, (double)n / elapsed, elapsed * 1e9 / (double)n);
    return 0;
}
