#define _POSIX_C_SOURCE 200809L
/* Proves the gap identified in review: cwist_io_queue_run() drives a
 * long-lived pooled worker thread (exactly like scheduler.c's worker
 * threads) that never exits between jobs. A forgotten cwist_free() must
 * therefore be caught per-job (cwist_gc_scope_flush() wired into
 * io_queue.c's run loop) rather than only at thread exit -- otherwise it
 * would sit unreleased for the worker's entire lifetime.
 *
 * The test submits many jobs that each "forget" to free a block and, from
 * inside those jobs (so it inspects the worker thread's own TLS state,
 * not the submitting thread's), records the high-water mark of
 * cwist_gc_scope_pending_count(). If the per-job flush is wired up, that
 * count can only ever be 0 or 1 -- the flush after each job's func(arg)
 * runs sees the count reset before the next job's func(arg) runs. If the
 * flush were missing (thread-exit sweep only), the count would grow
 * without bound as jobs pile up on the still-alive worker thread.
 */
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/gc.h>
#include <cwist/sys/io/cwist_io.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#define JOB_COUNT 5000

static atomic_size_t g_jobs_done = 0;
static atomic_size_t g_max_pending = 0;

static void forget_job(void *arg) {
    (void)arg;
    void *p = cwist_alloc(64);
    assert(p != NULL);
    /* Deliberately not freed -- this is the "developer forgot" case. */

    size_t pending = cwist_gc_scope_pending_count();
    size_t prev = atomic_load_explicit(&g_max_pending, memory_order_relaxed);
    while (pending > prev &&
           !atomic_compare_exchange_weak_explicit(&g_max_pending, &prev, pending,
                                                   memory_order_relaxed,
                                                   memory_order_relaxed)) {
        /* retry with updated prev */
    }
    atomic_fetch_add_explicit(&g_jobs_done, 1, memory_order_release);
}

static void *run_queue(void *arg) {
    cwist_io_queue_run((cwist_io_queue *)arg);
    return NULL;
}

int main(void) {
    cwist_full_gc(true);

    cwist_io_queue *q = cwist_io_queue_create(0);
    assert(q != NULL);

    pthread_t worker;
    assert(pthread_create(&worker, NULL, run_queue, q) == 0);

    for (int i = 0; i < JOB_COUNT; i++) {
        assert(cwist_io_queue_submit(q, forget_job, NULL));
    }

    /* Wait for every job to run, on the *same* worker thread the whole
     * time -- it must never exit during this loop. */
    while (atomic_load_explicit(&g_jobs_done, memory_order_acquire) < JOB_COUNT) {
        struct timespec ts = {0, 1000000}; /* 1ms */
        nanosleep(&ts, NULL);
    }

    /* The worker thread is still alive and has processed JOB_COUNT jobs.
     * If per-job flush is wired up, at most the current job's own block was
     * ever pending when measured. If it were only swept at thread exit,
     * this would have climbed to JOB_COUNT. */
    size_t max_pending = atomic_load(&g_max_pending);
    printf("test_io_queue_full_gc: max_pending=%zu (worker thread still alive)\n", max_pending);
    assert(max_pending <= 1);

    cwist_io_queue_stop(q);
    pthread_join(worker, NULL);
    cwist_io_queue_destroy(q);

    cwist_full_gc(false);
    printf("test_io_queue_full_gc: ok\n");
    return 0;
}
