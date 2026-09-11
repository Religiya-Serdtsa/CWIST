#define _POSIX_C_SOURCE 200809L
/* Exercises the "exactly-once release" pattern for a value shared by two
 * sibling references that can be torn down from either side concurrently:
 *
 *   holder.ref_a and holder.ref_b both point at the same heap block. Either
 *   side may decide to release it first, and only one of them may actually
 *   detach the pair and free the shared block; the other must back off.
 *
 * The release itself goes through cwist_ebr_free() instead of cwist_free()
 * so that a reader on some other thread, wrapped in
 * ttak_epoch_enter()/ttak_epoch_exit(), can never observe a freed block --
 * the real free is deferred until cwist_gc_pipeline_tick() confirms every
 * thread has moved past the epoch the release happened in.
 */
#include <cwist/core/mem/gc.h>
#include <ttak/mem/epoch.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    void *ref_a;
    void *ref_b;
} dual_ref_t;

typedef struct {
    dual_ref_t *holder;
    cwist_release_guard_t *guard;
    atomic_int *release_count;
} racer_arg_t;

/* One racer's attempt to tear the shared block down. Whichever of the two
 * concurrent callers wins cwist_release_guard_acquire() performs the
 * detach + deferred free; the loser is a no-op. */
static void *racer_main(void *arg) {
    racer_arg_t *a = (racer_arg_t *)arg;

    if (cwist_release_guard_acquire(a->guard)) {
        void *shared = a->holder->ref_a;
        a->holder->ref_a = NULL;
        a->holder->ref_b = NULL;
        atomic_fetch_add(a->release_count, 1);
        cwist_ebr_free(shared);
    }

    /* Short-lived helper thread: leave the epoch's active-thread list so it
     * cannot block a future ttak_epoch_reclaim() forever. */
    ttak_epoch_deregister_thread();
    return NULL;
}

/* A reader that repeatedly accesses the shared block through holder.ref_a
 * while the two racers above may be tearing it down. Every access is
 * wrapped in ttak_epoch_enter()/ttak_epoch_exit() so cwist_ebr_free()'s
 * deferred release can never land while this thread is mid-read. */
static void *reader_main(void *arg) {
    dual_ref_t *holder = (dual_ref_t *)arg;
    for (int i = 0; i < 1000; i++) {
        ttak_epoch_enter();
        void *seen = holder->ref_a;
        if (seen) {
            /* Touch the memory to make a use-after-free observable under
             * a sanitizer if the deferred-free guarantee ever broke. */
            volatile char probe = *(volatile char *)seen;
            (void)probe;
        }
        ttak_epoch_exit();
    }
    ttak_epoch_deregister_thread();
    return NULL;
}

int main(void) {
    cwist_release_guard_t guard;
    atomic_int release_count = 0;

    void *shared = cwist_ebr_alloc(sizeof(int));
    assert(shared != NULL);
    *(int *)shared = 42;

    dual_ref_t holder = { .ref_a = shared, .ref_b = shared };
    cwist_release_guard_init(&guard);

    racer_arg_t arg_a = { &holder, &guard, &release_count };
    racer_arg_t arg_b = { &holder, &guard, &release_count };

    pthread_t reader, racer_a, racer_b;
    assert(pthread_create(&reader, NULL, reader_main, &holder) == 0);
    assert(pthread_create(&racer_a, NULL, racer_main, &arg_a) == 0);
    assert(pthread_create(&racer_b, NULL, racer_main, &arg_b) == 0);

    pthread_join(reader, NULL);
    pthread_join(racer_a, NULL);
    pthread_join(racer_b, NULL);

    /* Exactly one of the two racers must have won the guard. */
    assert(atomic_load(&release_count) == 1);
    assert(holder.ref_a == NULL);
    assert(holder.ref_b == NULL);

    /* Drive the EBR pipeline until the deferred free actually runs. Every
     * racer/reader thread already deregistered above, so this converges
     * in a bounded number of ticks. */
    for (int i = 0; i < 16; i++) {
        cwist_gc_pipeline_tick();
    }

    printf("test_gc_ebr_release: ok (released=%d)\n", atomic_load(&release_count));
    return 0;
}
