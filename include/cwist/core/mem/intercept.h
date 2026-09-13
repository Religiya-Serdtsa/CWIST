/**
 * @file intercept.h
 * @brief Opt-in, translation-unit-scoped malloc/calloc/realloc/free
 *        interception onto the full-GC epoch pipeline.
 *
 * Design rationale (see docs/GC.md section 5 for the full writeup): this is
 * deliberately a *header-level* macro redefinition, not a link-level
 * `-Wl,--wrap=malloc`. A link-level wrap intercepts every `malloc`
 * reference in the final binary, including vendored dependencies
 * (BoringSSL, lsquic, cnats, sqlite3) that never see CWIST's headers and
 * have no reason to expect a non-libc allocator underneath them -
 * concretely, BoringSSL calls `OPENSSL_cleanse()` on secret key material
 * before freeing it, on the assumption of plain heap semantics; routing
 * that through an epoch-deferred GC arena would be a security regression,
 * not a performance footnote.
 *
 * A `#define malloc cwist_malloc_shim` here only ever takes effect in a
 * translation unit that (a) includes this header and (b) defines
 * `CWIST_INTERCEPT_MALLOC` before doing so. Vendored dependencies do
 * neither, structurally: they are built as separate compilation units that
 * never include a CWIST header. This excludes them by construction, at
 * the cost of not extending coverage beyond code that opts in.
 *
 * Usage (typically a handler translation unit that wants "write malloc by
 * habit and it still evaporates under cwist_full_gc(true)"):
 * @code
 *   #define CWIST_INTERCEPT_MALLOC
 *   #include <cwist/core/mem/intercept.h>
 *   #include <cwist/app.h>
 *
 *   static void handle(cwist_http_request *req, cwist_http_response *res) {
 *       char *buf = malloc(256);   // -> cwist_malloc_shim() under the hood
 *       ...
 *       // no free(buf): under cwist_full_gc(true), this evaporates when
 *       // the current thread's pending-sweep list is flushed/thread exits,
 *       // exactly like a forgotten cwist_alloc() would. free(buf), if
 *       // called, still works normally either way.
 *   }
 * @endcode
 *
 * When `cwist_full_gc(true)` was never called (the default), every shim
 * function below falls straight through to the real libc call after one
 * relaxed atomic load (`cwist_full_gc_enabled()`, the same check
 * `cwist_alloc()`/`cwist_free()` already pay on every call) - no tracking,
 * no extra bookkeeping. See tests/bench_malloc_intercept.c for the
 * measured overhead of that fallback path, both with the toggle on and
 * off, against a plain uninterecepted malloc/free baseline.
 *
 * A pointer that legitimately needs to outlive the thread/scope that
 * allocated it (cached in a connection pool, hand it to a background job)
 * calls `cwist_gc_scope_disown(ptr)` once at the handoff point, exactly as
 * it would for a `cwist_alloc()` pointer - see
 * `include/cwist/core/mem/gc.h` and `tests/test_full_gc_ownership_handoff.c`.
 * No separate escape API is needed for shimmed pointers.
 */
#ifndef __CWIST_CORE_MEM_INTERCEPT_H__
#define __CWIST_CORE_MEM_INTERCEPT_H__

#include <stddef.h>
/* Must pull in <stdlib.h> here, before the malloc/calloc/realloc/free
 * macros below take effect: if a later #include <stdlib.h> (by this
 * translation unit or transitively by another header) were the first one
 * processed, its own `void *calloc(size_t, size_t);`-style declarations
 * would get rewritten by our function-like macros mid-declaration - not a
 * hypothetical, this reliably fails to compile. Getting stdlib.h's
 * include guard to trip on any later inclusion (a no-op once already
 * included) avoids that class of corruption entirely. */
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief malloc() replacement: real libc malloc() (uninitialized memory,
 *        exactly like malloc - unlike cwist_alloc(), this does not zero),
 *        registered with the full-GC pending-sweep list when
 *        cwist_full_gc_enabled(); a plain, untracked passthrough otherwise.
 */
void *cwist_malloc_shim(size_t size);

/**
 * @brief calloc() replacement: real libc calloc() (zeroed), registered
 *        with the full-GC pending-sweep list when cwist_full_gc_enabled();
 *        a plain, untracked passthrough otherwise.
 */
void *cwist_calloc_shim(size_t nmemb, size_t size);

/**
 * @brief realloc() replacement. Tracking follows the incoming pointer: if
 *        @p ptr was tracked (shimmed-and-full-GC-was-on at allocation
 *        time), the old registration is removed and the new pointer is
 *        re-registered (a moved block must not leave a stale entry
 *        pointing at freed memory, and should stay reclaimable exactly as
 *        the original was). If @p ptr was untracked (allocated before
 *        full-GC was enabled, already disowned, or is NULL), the result is
 *        left untracked too - matching cwist_realloc()'s existing
 *        behavior of not opting a pointer into tracking on its own.
 */
void *cwist_realloc_shim(void *ptr, size_t size);

/**
 * @brief free() replacement: untracks @p ptr first if it was tracked
 *        (safe no-op if it wasn't - same contract as cwist_free()), then
 *        calls the real libc free().
 */
void cwist_free_shim(void *ptr);

#ifdef __cplusplus
}
#endif

#if defined(CWIST_INTERCEPT_MALLOC)
#define malloc(sz) cwist_malloc_shim(sz)
#define calloc(n, sz) cwist_calloc_shim((n), (sz))
#define realloc(p, sz) cwist_realloc_shim((p), (sz))
#define free(p) cwist_free_shim(p)
#endif

#endif
