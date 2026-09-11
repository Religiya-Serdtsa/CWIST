#define _POSIX_C_SOURCE 200809L
/* Covers full-GC's connection registry (docs/GC.md sections 2-3): a
 * handle explicitly untrack()'d before its normal close is never
 * double-closed by a sweep; a handle left tracked when its owning thread
 * exits is closed exactly once by that thread's TLS destructor; and a
 * handle left tracked when the *process* exits (other threads still
 * "running" in the sense their TLS destructors never get a chance to
 * fire) is still closed by the atexit sweep -- verified via a real
 * subprocess so atexit() actually runs.
 */
#include <cwist/core/mem/gc.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static atomic_int g_close_count = 0;

static void close_cb(void *handle) {
    atomic_fetch_add(&g_close_count, 1);
    free(handle);
}

/* --- explicit untrack must prevent the sweep from touching it --- */
static void test_untrack_prevents_sweep(void) {
    cwist_full_gc(true);
    void *h = malloc(1);
    cwist_conn_registry_track(h, close_cb);
    assert(cwist_conn_registry_untrack(h) == true);
    /* Second untrack: not found (already removed). */
    assert(cwist_conn_registry_untrack(h) == false);
    cwist_conn_registry_flush(); /* would double-close h if untrack didn't work */
    free(h);
    assert(atomic_load(&g_close_count) == 0);
    printf("test_conn_registry: untrack ok\n");
}

/* --- a forgotten handle is closed once when its owning thread exits --- */
static void *thread_forgets_one(void *arg) {
    (void)arg;
    void *h = malloc(1);
    cwist_conn_registry_track(h, close_cb);
    /* no untrack -- forgotten on purpose */
    return NULL;
}

static void test_thread_exit_sweep(void) {
    int before = atomic_load(&g_close_count);
    pthread_t t;
    assert(pthread_create(&t, NULL, thread_forgets_one, NULL) == 0);
    assert(pthread_join(t, NULL) == 0);
    int after = atomic_load(&g_close_count);
    assert(after - before == 1);
    printf("test_conn_registry: thread-exit sweep ok\n");
}

/* --- process-exit sweep: run in a child process so atexit() fires for
 * real, with another thread still "alive" (never joined) holding a
 * forgotten handle -- its own TLS destructor never runs in that case,
 * only the atexit path can close it. */
static void *thread_forgets_and_blocks(void *arg) {
    void *h = malloc(1);
    cwist_conn_registry_track(h, close_cb);
    pause(); /* never returns; this thread is still "running" at exit() */
    (void)arg;
    return NULL;
}

/* Registered first (before the tracking thread even starts), so LIFO
 * atexit ordering runs it *last* -- after the connection registry's own
 * atexit sweep has already run and updated g_close_count. */
static void print_close_count_atexit(void) {
    printf("atexit-swept:%d\n", atomic_load(&g_close_count));
    fflush(stdout);
}

static int child_main(void) {
    atexit(print_close_count_atexit);
    cwist_full_gc(true);
    pthread_t t;
    if (pthread_create(&t, NULL, thread_forgets_and_blocks, NULL) != 0) return 2;
    struct timespec ts = {0, 50 * 1000 * 1000}; /* let it track before we exit */
    nanosleep(&ts, NULL);
    exit(0); /* runs atexit handlers; thread_forgets_and_blocks() never returns */
}

static void test_process_exit_sweep(void) {
    /* Use a pipe so the child can report whether close_cb() ran, since
     * process-local state doesn't survive fork(). */
    int fd[2];
    assert(pipe(fd) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(fd[0]);
        dup2(fd[1], STDOUT_FILENO);
        child_main();
        _exit(3); /* unreachable: child_main() calls exit() */
    }
    close(fd[1]);
    char buf[256] = {0};
    ssize_t n = read(fd[0], buf, sizeof(buf) - 1);
    close(fd[0]);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(n > 0 && strstr(buf, "atexit-swept:1") != NULL);
    printf("test_conn_registry: process-exit sweep ok\n");
}

int main(void) {
    test_untrack_prevents_sweep();
    test_thread_exit_sweep();
    test_process_exit_sweep();
    printf("test_conn_registry: ok\n");
    return 0;
}
