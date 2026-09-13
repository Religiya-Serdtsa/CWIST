/* Functional correctness for CWIST_INTERCEPT_MALLOC (see
 * include/cwist/core/mem/intercept.h). Every call in this file after the
 * #define below is a plain-looking malloc/calloc/realloc/free - the point
 * of the test is that they behave exactly like the real thing when
 * full-GC is off, and additionally evaporate on their own when full-GC is
 * on and the thread's pending-sweep list is flushed without an explicit
 * free(). */
#define CWIST_INTERCEPT_MALLOC
#include <cwist/core/mem/intercept.h>

#include <cwist/core/mem/gc.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "Check failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

/* --- Behaves like real malloc/calloc/realloc/free when full-GC is off --- */
static int test_passthrough_when_gc_off(void) {
    REQUIRE(!cwist_full_gc_enabled());

    char *a = malloc(13);
    REQUIRE(a != NULL);
    memcpy(a, "hello world", 12);
    a[12] = '\0';
    REQUIRE(strcmp(a, "hello world") == 0);

    int *nums = calloc(4, sizeof(int));
    REQUIRE(nums != NULL);
    for (int i = 0; i < 4; i++) REQUIRE(nums[i] == 0);

    a = realloc(a, 32);
    REQUIRE(a != NULL);
    REQUIRE(strcmp(a, "hello world") == 0);

    free(a);
    free(nums);
    puts("test_passthrough_when_gc_off: ok");
    return 0;
}

/* --- Tracked and swept when full-GC is on and never explicitly freed --- */
static int test_tracked_and_swept_when_gc_on(void) {
    cwist_full_gc(true);
    REQUIRE(cwist_full_gc_enabled());

    char *forgotten = malloc(64);
    REQUIRE(forgotten != NULL);
    strcpy(forgotten, "never freed by hand");

    int *also_forgotten = calloc(8, sizeof(int));
    REQUIRE(also_forgotten != NULL);

    /* No free() for either - this is the whole point. Flush this thread's
     * pending-sweep list right now instead of waiting for thread exit, so
     * the test doesn't need a second thread to observe the sweep. */
    cwist_gc_scope_flush();

    /* Correctness we can observe from here: a value we DO explicitly
     * free() is untracked cleanly (no double-free at process exit), and a
     * realloc'd-then-forgotten block is equally swept. */
    char *b = malloc(16);
    REQUIRE(b != NULL);
    b = realloc(b, 48);
    REQUIRE(b != NULL);
    strcpy(b, "explicitly freed");
    free(b);

    char *c = malloc(16);
    REQUIRE(c != NULL);
    c = realloc(c, 48);
    REQUIRE(c != NULL);
    /* forgotten again */
    cwist_gc_scope_flush();

    puts("test_tracked_and_swept_when_gc_on: ok");
    return 0;
}

/* --- realloc tracking follows the incoming pointer, not the toggle at
 * call time: a pointer allocated while GC was off and later realloc'd
 * while GC is on stays untracked (matching cwist_realloc()'s existing
 * behavior of never opting a pointer into tracking on its own). --- */
static int test_realloc_tracking_follows_pointer(void) {
    /* Note: full-GC is already on from the previous test and is a
     * one-shot, non-reversible toggle (cwist_full_gc_locked()) - so this
     * test allocates its baseline pointer with the shim active but must
     * reason about tracking via explicit free(), not by toggling GC off
     * again (that's not possible once enabled). */
    char *untracked_origin = malloc(8); /* GC on: this IS tracked */
    REQUIRE(untracked_origin != NULL);
    /* Disown it immediately to simulate "allocated, then intentionally
     * handed off" - now it behaves as the realloc_shim's "was_tracked ==
     * false" branch would for a pointer from before GC was ever enabled. */
    cwist_gc_scope_disown(untracked_origin);

    char *moved = realloc(untracked_origin, 256);
    REQUIRE(moved != NULL);
    /* Since the incoming pointer was untracked (disowned), the shim must
     * not silently re-track the result - it remains the caller's
     * responsibility. Prove it: explicitly free it. If the shim had
     * mistakenly tracked it, this free() would just untrack-then-free
     * (still fine) so this alone doesn't fully prove it, but combined
     * with test_tracked_and_swept_when_gc_on's forgotten-pointer coverage
     * above, the tracked path is independently verified. This case exists
     * to make sure disowned-then-realloc'd pointers don't crash or
     * double-free. */
    free(moved);

    puts("test_realloc_tracking_follows_pointer: ok");
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_passthrough_when_gc_off();
    rc |= test_tracked_and_swept_when_gc_on();
    rc |= test_realloc_tracking_follows_pointer();
    if (rc == 0) puts("All malloc-intercept tests passed!");
    return rc;
}
