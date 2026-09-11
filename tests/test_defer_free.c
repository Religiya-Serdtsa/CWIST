#define _POSIX_C_SOURCE 200809L
/* Covers CWIST_DEFER_FREE / cwist_alloc_scoped(): a block-scope cleanup
 * attribute that frees a cwist_alloc() block automatically on every return
 * path out of the block it is declared in. This is the safe complement to
 * cwist_gc_scope_track()/cwist_gc_scope_disown(): it only ever applies to
 * a pointer that provably does not escape its declaring block, so unlike
 * the dynamic full-GC tracking, there is nothing here that needs auditing
 * for cross-job/cross-thread handoff -- the compiler enforces the scope.
 */
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/gc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Multiple return paths -- every one of them must still free buf. */
static int multi_return(int which) {
    void *buf CWIST_DEFER_FREE = cwist_alloc(64);
    assert(buf != NULL);
    memset(buf, 0xAB, 64);

    if (which == 0) return 0;      /* freed here */
    if (which == 1) return 1;      /* freed here */
    if (which == 2) {
        return 2;                  /* freed here, from inside a nested block */
    }
    return -1;                     /* freed here */
}

/* cwist_alloc_scoped() convenience form. */
static void scoped_macro_form(void) {
    cwist_alloc_scoped(name, char *, 64);
    assert(name != NULL);
    snprintf(name, 64, "scoped-%d", 42);
    assert(strcmp(name, "scoped-42") == 0);
    /* freed automatically here */
}

/* Under full-GC mode, cwist_free() (which the cleanup callback calls)
 * already untracks from the pending-sweep list -- so CWIST_DEFER_FREE
 * should interoperate with full-GC tracking with no special-casing: the
 * pending count must be back to 0 once the block exits. */
static void scoped_under_full_gc(void) {
    {
        void *buf CWIST_DEFER_FREE = cwist_alloc(128);
        assert(buf != NULL);
        assert(cwist_gc_scope_pending_count() == 1);
    }
    assert(cwist_gc_scope_pending_count() == 0);
}

int main(void) {
    for (int i = -5; i <= 3; i++) {
        (void)multi_return(i);
    }
    scoped_macro_form();

    cwist_full_gc(true);
    scoped_under_full_gc();
    cwist_full_gc(false);

    printf("test_defer_free: ok\n");
    return 0;
}
