#define _POSIX_C_SOURCE 200809L
/* Covers the hardened cwist_full_gc() toggle: it must be settable exactly
 * once (a later call -- benign re-invocation or an attacker with a
 * call-injection primitive -- is a silent no-op), and its backing memory
 * must actually become unwritable afterwards (a raw arbitrary-write
 * primitive that bypasses the API and targets the flag's memory directly
 * must hard-fault instead of silently flipping it).
 */
#include <cwist/core/mem/gc.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

/* --- Part 1: the one-time latch, in-process --- */
static void test_latch(void) {
    assert(!cwist_full_gc_locked());

    cwist_full_gc(true);
    assert(cwist_full_gc_locked());
    assert(cwist_full_gc_enabled());

    /* A later call -- however it happens -- must not change anything. */
    cwist_full_gc(false);
    assert(cwist_full_gc_enabled());
    cwist_full_gc(true);
    assert(cwist_full_gc_enabled());

    printf("test_full_gc_toggle_hardening: latch ok (locked after first call, "
           "later calls ignored)\n");
}

/* --- Part 2: the guard page is actually read-only after the first call,
 * proven by forking a child that tries to write it directly and
 * confirming the child dies with SIGSEGV/SIGBUS rather than succeeding. */
static void test_page_hardening(void) {
    /* Parent already locked the guard in test_latch(); the child inherits
     * that state via fork() (same mapped page, copy-on-write semantics
     * don't matter here since we're about to fault on write anyway). */
    volatile char *raw = (volatile char *)cwist_full_gc_guard_page();
    assert(raw != NULL);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /* Child: attempt a raw write to the guard page, bypassing
         * cwist_full_gc() entirely -- this must crash, not succeed. */
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        *raw = 0x41;
        /* If we get here, the page was writable -- hardening failed. */
        _exit(0);
    }

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status));
    int sig = WTERMSIG(status);
    assert(sig == SIGSEGV || sig == SIGBUS);

    printf("test_full_gc_toggle_hardening: page-hardening ok "
           "(direct write crashed with signal %d, as expected)\n", sig);
}

int main(void) {
    test_latch();
    test_page_hardening();
    printf("test_full_gc_toggle_hardening: ok\n");
    return 0;
}
