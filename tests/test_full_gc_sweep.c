#define _POSIX_C_SOURCE 200809L
/* Full-GC smoke test: a worker thread allocates via cwist_alloc(), frees
 * half of them explicitly, and "forgets" the other half before exiting.
 * With cwist_full_gc(true), the forgotten half must be picked up by the
 * thread-exit sweep (cwist_gc_scope_track/untrack + the TLS destructor in
 * gc.c) instead of leaking, and the explicit-free half must still work
 * exactly as before. */
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/gc.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

static void *worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 100; i++) {
        void *p = cwist_alloc(32);
        assert(p != NULL);
        if (i % 2 == 0) {
            cwist_free(p); /* explicit path: must not be swept again later */
        }
        /* the other half is deliberately "forgotten" */
    }
    return NULL; /* thread exit should sweep the 50 forgotten blocks */
}

int main(void) {
    cwist_full_gc(true);
    assert(cwist_full_gc_enabled());

    pthread_t t;
    assert(pthread_create(&t, NULL, worker, NULL) == 0);
    pthread_join(t, NULL);

    for (int i = 0; i < 16; i++) cwist_gc_pipeline_tick();

    /* Full-GC mode must not break the plain explicit-free path either. */
    void *p = cwist_alloc(16);
    assert(p != NULL);
    cwist_free(p);

    cwist_full_gc(false);
    printf("test_full_gc_smoke: ok\n");
    return 0;
}
