#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* Hold the first idle worker after its condition wait released the queue
 * mutex. This models a signalled worker that has not been scheduled yet.
 * Make pool threads joinable only in this harness so no detached worker can
 * outlive the synchronization objects during test cleanup. */
static int delayed_wait(pthread_cond_t *, pthread_mutex_t *);
static int joinable_create(pthread_t *, const pthread_attr_t *,
                           void *(*)(void *), void *);
#define pthread_cond_wait delayed_wait
#define pthread_create joinable_create
#include "../src/net/http/http.c"
#undef pthread_create
#undef pthread_cond_wait

#define JOBS 8
#define MAX_THREADS (JOBS * 4)
static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int initial_parked, initial_threads = 1;
static bool release_initial, release_handlers;
static int fail_creates, failed_creates;
static int started, finished;
static bool seen[JOBS];
static pthread_t threads[MAX_THREADS];
static int thread_count;

static struct timespec deadline(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_REALTIME, &ts) == 0);
    ts.tv_sec += 3;
    return ts;
}

static int delayed_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    if (cond == &g_dyn_pool.cond) {
        pthread_mutex_lock(&gate_mu);
        if (initial_parked < initial_threads) {
            initial_parked++;
            pthread_mutex_unlock(mutex);
            pthread_cond_broadcast(&gate_cv);
            while (!release_initial) pthread_cond_wait(&gate_cv, &gate_mu);
            pthread_mutex_unlock(&gate_mu);
            pthread_mutex_lock(mutex);
            return 0;
        }
        pthread_mutex_unlock(&gate_mu);
    }
    return pthread_cond_wait(cond, mutex);
}

static int joinable_create(pthread_t *thread, const pthread_attr_t *attr,
                           void *(*start)(void *), void *arg) {
    pthread_mutex_lock(&gate_mu);
    if (fail_creates) {
        fail_creates--;
        failed_creates++;
        pthread_mutex_unlock(&gate_mu);
        return EAGAIN;
    }
    pthread_mutex_unlock(&gate_mu);
    pthread_attr_t joinable;
    assert(pthread_attr_init(&joinable) == 0);
    size_t stack_size;
    assert(pthread_attr_getstacksize(attr, &stack_size) == 0);
    assert(pthread_attr_setstacksize(&joinable, stack_size) == 0);
    int rc = pthread_create(thread, &joinable, start, arg);
    pthread_attr_destroy(&joinable);
    if (!rc) {
        pthread_mutex_lock(&gate_mu);
        assert(thread_count < MAX_THREADS);
        threads[thread_count++] = *thread;
        pthread_mutex_unlock(&gate_mu);
    }
    return rc;
}

static void held_handler(int fd, void *ctx) {
    (void)fd;
    int index = *(int *)ctx;
    pthread_mutex_lock(&gate_mu);
    assert(index >= 0 && index < JOBS && !seen[index]);
    seen[index] = true;
    started++;
    pthread_cond_broadcast(&gate_cv);
    while (!release_handlers) pthread_cond_wait(&gate_cv, &gate_mu);
    finished++;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_mu);
}

static void *submit_one(void *id) {
    cwist_http_pool_submit(-1, held_handler, id);
    return NULL;
}

int main(int argc, char **argv) {
    alarm(15);
    const char *mode = argc > 1 ? argv[1] : "burst";
    bool capped = strcmp(mode, "cap") == 0;
    bool failure = strcmp(mode, "failure") == 0;
    initial_threads = strcmp(mode, "multi") == 0 ? 3 : 1;
    int expected = capped ? 4 : failure ? JOBS - 1 : JOBS;
    cwist_http_pool_limit_core((unsigned int)initial_threads);
    assert(setenv("CWIST_C1M_MODE", "0", 1) == 0);
    assert(setenv("CWIST_WORKER_THREADS", "1", 1) == 0);
    assert(setenv("CWIST_POOL_IDLE_TIMEOUT_MS", "0", 1) == 0);
    assert(unsetenv("CWIST_POOL_PREWARM") == 0);
    assert(cwist_http_pool_init() == 0);
    pthread_mutex_lock(&gate_mu);
    struct timespec until = deadline();
    while (initial_parked < initial_threads) {
        int rc = pthread_cond_timedwait(&gate_cv, &gate_mu, &until);
        assert(rc == 0);
    }
    if (capped) atomic_store(&g_dyn_pool.max_workers, 4);
    if (failure) fail_creates = 2;
    pthread_mutex_unlock(&gate_mu);

    int ids[JOBS];
    pthread_t submitters[JOBS];
    for (int i = 0; i < JOBS; i++) {
        ids[i] = i;
        /* The handler owns no transport in this queue-only regression. */
        if (capped) assert(pthread_create(&submitters[i], NULL, submit_one, &ids[i]) == 0);
        else submit_one(&ids[i]);
    }
    if (capped) for (int i = 0; i < JOBS; i++) assert(pthread_join(submitters[i], NULL) == 0);
    pthread_mutex_lock(&gate_mu);
    release_initial = true;
    pthread_cond_broadcast(&gate_cv);
    until = deadline();
    while (started < expected) {
        int rc = pthread_cond_timedwait(&gate_cv, &gate_mu, &until);
        if (rc == ETIMEDOUT) break;
        assert(rc == 0);
    }
    int started_without_release = started;
    /* Release only after observing the contract; otherwise one worker could
     * run all eight handlers serially and disguise the starvation. */
    release_handlers = true;
    pthread_cond_broadcast(&gate_cv);
    until = deadline();
    while (finished < JOBS) {
        int rc = pthread_cond_timedwait(&gate_cv, &gate_mu, &until);
        assert(rc == 0);
    }
    pthread_mutex_unlock(&gate_mu);

    pthread_mutex_lock(&g_dyn_pool.lock);
    atomic_store(&g_dyn_pool.running, false);
    pthread_cond_broadcast(&g_dyn_pool.cond);
    pthread_mutex_unlock(&g_dyn_pool.lock);
    for (int i = 0; i < thread_count; i++) assert(pthread_join(threads[i], NULL) == 0);
    assert(atomic_load(&g_http_inflight) == 0);
    assert(atomic_load(&g_dyn_pool.pending_tasks) == 0);
    assert(atomic_load(&g_dyn_pool.active_workers) == thread_count);
    if (capped) assert(thread_count <= 4);
    if (failure) assert(failed_creates == 2 && fail_creates == 0);
    cwist_http_pool_destroy();
    printf("classic burst: %d/%d handlers started before any completion; workers=%d\n",
           started_without_release, JOBS, thread_count);
    if (started_without_release != expected) {
        fputs("FAIL: queued connections starve behind held keep-alive handlers\n", stderr);
        return 1;
    }
    puts("classic burst scaling: PASS");
    return 0;
}
