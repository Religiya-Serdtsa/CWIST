#define _POSIX_C_SOURCE 200809L
/* Covers cross-job ownership handoff under full-GC mode. job1 allocates a
 * block and stores it in a shared slot for job2 to pick up and free later
 * -- an ordinary handoff pattern (e.g. build a response buffer in one
 * callback, write it out in a later one). Without cwist_gc_scope_disown(),
 * job1 never calling cwist_free() on it would make the per-job
 * cwist_gc_scope_flush() wired into io_queue.c auto-sweep it anyway, and
 * the existing reclaim cadence would actually free it before job2 ever
 * touches it. cwist_gc_scope_disown() at the handoff point prevents that:
 * it removes the block from job1's pending-sweep list without freeing it,
 * so job2 remains the sole, later owner.
 */
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/gc.h>
#include <cwist/sys/io/cwist_io.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static void *_Atomic g_handoff_slot = NULL;
static atomic_int g_job1_done = 0;
static atomic_int g_job2_done = 0;
static atomic_int g_job2_saw_corruption = 0;

static void job1_produce(void *arg) {
    (void)arg;
    char *a = (char *)cwist_alloc(32);
    assert(a != NULL);
    memcpy(a, "still-alive", strlen("still-alive") + 1);

    /* Intentional handoff: job2 owns it from here and will cwist_free it.
     * Remove it from this thread's pending-sweep list first so the
     * per-job auto-flush does not treat it as forgotten and release it
     * out from under job2. */
    cwist_gc_scope_disown(a);
    atomic_store_explicit(&g_handoff_slot, a, memory_order_release);
    /* No cwist_free(a) here -- ownership was transferred, not forgotten. */
    atomic_store_explicit(&g_job1_done, 1, memory_order_release);
}

static void job2_consume(void *arg) {
    (void)arg;
    while (!atomic_load_explicit(&g_job1_done, memory_order_acquire)) {
        struct timespec ts = {0, 100000};
        nanosleep(&ts, NULL);
    }
    char *a = (char *)atomic_load_explicit(&g_handoff_slot, memory_order_acquire);
    assert(a != NULL);
    if (strcmp(a, "still-alive") != 0) {
        atomic_store_explicit(&g_job2_saw_corruption, 1, memory_order_release);
    }
    cwist_free(a);
    atomic_store_explicit(&g_job2_done, 1, memory_order_release);
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

    assert(cwist_io_queue_submit(q, job1_produce, NULL));
    assert(cwist_io_queue_submit(q, job2_consume, NULL));

    while (!atomic_load_explicit(&g_job2_done, memory_order_acquire)) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }

    cwist_io_queue_stop(q);
    pthread_join(worker, NULL);
    cwist_io_queue_destroy(q);
    cwist_full_gc(false);

    if (atomic_load(&g_job2_saw_corruption)) {
        fprintf(stderr, "test_full_gc_ownership_handoff: FAILED -- job2 read "
                        "corrupted/freed memory that job1 handed off\n");
        return 1;
    }
    printf("test_full_gc_ownership_handoff: ok\n");
    return 0;
}
