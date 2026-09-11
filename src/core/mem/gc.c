#include <cwist/core/mem/gc.h>
#include <cwist/core/mem/alloc.h>
#include <ttak/mem/epoch.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

/**
 * @file gc.c
 * @brief Thin implementation wrapper around libttak epoch-based reclamation.
 */

/**
 * @brief Lazily initialize and optionally configure manual rotation mode.
 * @param gc GC context owned by the caller.
 * @param manual_rotation When true, keep epoch advancement under explicit caller control.
 */
void cwist_gc(cwist_gc_t *gc, bool manual_rotation) {
    if (!gc) return;
    if (!gc->initialized) {
        ttak_epoch_gc_init(&gc->impl);
        gc->initialized = true;
    }
    ttak_epoch_gc_manual_rotate(&gc->impl, manual_rotation);
}

/**
 * @brief Destroy the wrapped libttak GC state when it has been initialized.
 * @param gc GC context to shut down.
 */
void cwist_gc_shutdown(cwist_gc_t *gc) {
    if (!gc || !gc->initialized) return;
    ttak_epoch_gc_destroy(&gc->impl);
    gc->initialized = false;
}

/**
 * @brief Advance the current epoch and reclaim retired nodes when eligible.
 * @param gc GC context to rotate.
 */
void cwist_gc_rotate(cwist_gc_t *gc) {
    if (!gc || !gc->initialized) return;
    ttak_epoch_gc_rotate(&gc->impl);
}

/**
 * @brief Enable auto-rotation of epoch GC.
 * @param gc GC context to enable auto-rotation.
 */
void cwist_gc_auto_rotate(cwist_gc_t *gc, bool enabled) {
    if(!gc || !gc->initialized) return;
    bool gc_status = atomic_load(&gc->auto_rotated);
    while(!atomic_compare_exchange_weak(&gc->auto_rotated, &gc_status, enabled));
}

/**
 * @brief Return if epoch GC is auto-rotated.
 * @ param gc GC context to get status.
 */
bool cwist_gc_auto_rotated(cwist_gc_t *gc) {
    return atomic_load(&gc->auto_rotated);
}

/**
 * @brief Register a pointer with zero-size accounting metadata.
 * @param gc GC context that tracks the pointer.
 * @param ptr Pointer to retire through the epoch GC.
 */
void cwist_reg_ptr(cwist_gc_t *gc, void *ptr) {
    cwist_reg_ptr_sized(gc, ptr, 0);
}

/**
 * @brief Register a pointer and its approximate size with the epoch GC.
 * @param gc GC context that tracks the pointer.
 * @param ptr Pointer to retire through the epoch GC.
 * @param size Optional size hint associated with @p ptr.
 */
void cwist_reg_ptr_sized(cwist_gc_t *gc, void *ptr, size_t size) {
    if (!gc || !gc->initialized || !ptr) return;
    ttak_epoch_gc_register(&gc->impl, ptr, size);
}

/**
 * @brief Expose the underlying libttak epoch GC structure for advanced integrations.
 * @param gc GC wrapper owned by CWIST.
 * @return Raw libttak GC handle, or NULL when the wrapper is unavailable.
 */
ttak_epoch_gc_t *cwist_gc_raw(cwist_gc_t *gc) {
    if (!gc || !gc->initialized) return NULL;
    return &gc->impl;
}

/**
 * @brief Allocate a block that will later be released through cwist_ebr_free().
 * @param size Number of bytes to allocate.
 * @return Newly allocated block, or NULL on failure.
 */
void *cwist_ebr_alloc(size_t size) {
    /* Nothing to protect yet: a block nobody else can see does not need
     * epoch coverage until it is published to another thread. */
    return cwist_alloc(size);
}

/**
 * @brief Callback libttak invokes once it is safe to actually free @p ptr.
 * @param ptr Block previously retired via cwist_ebr_free().
 */
static void cwist_ebr_free_cb(void *ptr) {
    cwist_free(ptr);
}

/**
 * @brief Defer release of a block until every thread that might still be
 *        reading it (inside ttak_epoch_enter()/ttak_epoch_exit()) has left.
 * @param ptr Block to retire; a no-op when NULL.
 */
void cwist_ebr_free(void *ptr) {
    if (!ptr) return;
    ttak_epoch_retire(ptr, cwist_ebr_free_cb);
}

/**
 * @brief Advance the EBR epoch and run any callbacks that are now safe.
 */
void cwist_gc_pipeline_tick(void) {
    ttak_epoch_reclaim();
}

/**
 * @brief Reset a release guard to its unclaimed state.
 * @param guard Guard to initialize.
 */
void cwist_release_guard_init(cwist_release_guard_t *guard) {
    if (!guard) return;
    atomic_store_explicit(guard, false, memory_order_relaxed);
}

/**
 * @brief Claim a release guard.
 * @param guard Guard shared by every racing caller.
 * @return true for exactly one caller across all racers; false otherwise.
 */
bool cwist_release_guard_acquire(cwist_release_guard_t *guard) {
    if (!guard) return false;
    bool expected = false;
    return atomic_compare_exchange_strong_explicit(guard, &expected, true,
                                                    memory_order_acq_rel,
                                                    memory_order_relaxed);
}

/* --- Full-GC mode: process-wide toggle + per-thread pending-sweep list --- */

/**
 * @brief Hardened backing storage for the full-GC toggle.
 *
 * Two attacks this defends against, since flipping this flag mid-run
 * silently disables the EBR deferred-free safety net full-GC mode
 * provides (opening a use-after-free window for anything it was
 * protecting): (1) a later call to cwist_full_gc() -- an accidental
 * re-invocation, or an attacker who has gained the ability to call
 * arbitrary exported functions -- and (2) a raw arbitrary-write
 * primitive that overwrites this flag's memory directly, bypassing the
 * API entirely.
 *
 * Defense: the struct lives alone on its own mmap()'d page. cwist_full_gc()
 * claims the right to set it exactly once via an atomic CAS on `locked`
 * (defeats (1): every later call, benign or hostile, is a silent no-op);
 * once set, the page is mprotect()'d PROT_READ (defeats (2): any write
 * that bypasses the CAS check and lands on this memory directly hard-faults
 * instead of silently succeeding).
 */
typedef struct {
    _Atomic bool enabled;
    _Atomic bool locked;
} cwist_full_gc_guard_t;

/**
 * @brief The guard page itself, or NULL if the mmap() below failed
 * (treated as full-GC being permanently unavailable, never as a security
 * regression -- see cwist_full_gc_enabled()).
 */
static cwist_full_gc_guard_t *g_full_gc_guard = NULL;

/**
 * @brief Map the guard page before main() runs, so cwist_full_gc_enabled()
 * -- called on every cwist_alloc()/cwist_free() -- never needs a
 * pthread_once/lazy-init check on its hot path (that check itself
 * regressed the C1M reactor latency gate in CI once already).
 */
__attribute__((constructor))
static void cwist_full_gc_guard_init(void) {
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    void *page = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return;
    cwist_full_gc_guard_t *guard = (cwist_full_gc_guard_t *)page;
    atomic_init(&guard->enabled, false);
    atomic_init(&guard->locked, false);
    g_full_gc_guard = guard;
}

/** @brief Process-wide GC instance backing full-GC's epoch-retire pipeline. */
static cwist_gc_t g_full_gc;
static pthread_once_t g_full_gc_once = PTHREAD_ONCE_INIT;

static void cwist_full_gc_lazy_init(void) {
    cwist_gc(&g_full_gc, true);
}

static cwist_gc_t *cwist_full_gc_instance(void) {
    pthread_once(&g_full_gc_once, cwist_full_gc_lazy_init);
    return &g_full_gc;
}

/**
 * @brief Serializes claiming the "first caller" slot in cwist_full_gc().
 *
 * NOT a CAS on g_full_gc_guard->locked: a CAS is a hardware read-modify-
 * WRITE (x86 cmpxchg asserts write intent even when the comparison
 * fails), so trying one on the guard page after it's been mprotect()'d
 * PROT_READ faults immediately -- on every later caller, exactly the
 * "harmless no-op" case this is supposed to handle gracefully. This
 * mutex lives off the guarded page, so contending for it is always safe;
 * only the thread that wins it ever writes to the page, and only before
 * that same thread mprotect()s it.
 */
static pthread_mutex_t g_full_gc_claim_mu = PTHREAD_MUTEX_INITIALIZER;

void cwist_full_gc(bool enable) {
    if (!g_full_gc_guard) return; /* guard page unavailable; fail safe, not silently unhardened */

    /* Fast path: once locked, this plain load is the only thing every
     * later caller ever does -- safe indefinitely, even after the page
     * below becomes read-only. */
    if (atomic_load_explicit(&g_full_gc_guard->locked, memory_order_acquire)) return;

    pthread_mutex_lock(&g_full_gc_claim_mu);
    if (atomic_load_explicit(&g_full_gc_guard->locked, memory_order_acquire)) {
        /* Someone else claimed it while we were waiting for the mutex. */
        pthread_mutex_unlock(&g_full_gc_claim_mu);
        return;
    }

    /* We hold the mutex and locked is still false: nobody has mprotect()'d
     * the page yet, so it is still writable and we are its sole writer. */
    atomic_store_explicit(&g_full_gc_guard->enabled, enable, memory_order_relaxed);
    atomic_store_explicit(&g_full_gc_guard->locked, true, memory_order_release);
    long page_size = sysconf(_SC_PAGESIZE);
    mprotect(g_full_gc_guard, (size_t)(page_size > 0 ? page_size : 4096), PROT_READ);
    pthread_mutex_unlock(&g_full_gc_claim_mu);

    cwist_gc_auto_rotate(cwist_full_gc_instance(), enable);
}

bool cwist_full_gc_enabled(void) {
    if (!g_full_gc_guard) return false;
    return atomic_load_explicit(&g_full_gc_guard->enabled, memory_order_relaxed);
}

bool cwist_full_gc_locked(void) {
    if (!g_full_gc_guard) return false;
    return atomic_load_explicit(&g_full_gc_guard->locked, memory_order_acquire);
}

void *cwist_full_gc_guard_page(void) {
    return g_full_gc_guard;
}

/** @brief One thread's list of cwist_alloc() blocks not yet cwist_free()'d. */
typedef struct {
    void **items;
    size_t count;
    size_t cap;
} cwist_gc_pending_t;

/**
 * @brief Retire every pending block in @p pending and reset the list to empty.
 * @param pending List to drain; a no-op when NULL.
 */
static void cwist_gc_pending_flush(cwist_gc_pending_t *pending) {
    if (!pending) return;
    for (size_t i = 0; i < pending->count; i++) {
        cwist_ebr_free(pending->items[i]);
    }
    pending->count = 0;
}

/**
 * @brief pthread TLS destructor: sweep whatever this thread never freed.
 * @param arg Thread-local cwist_gc_pending_t allocated by cwist_gc_pending_get().
 */
static void cwist_gc_pending_destroy(void *arg) {
    cwist_gc_pending_t *pending = (cwist_gc_pending_t *)arg;
    if (!pending) return;
    cwist_gc_pending_flush(pending);
    cwist_gc_pipeline_tick();
    free(pending->items);
    free(pending);
}

static pthread_key_t g_pending_key;
static pthread_once_t g_pending_key_once = PTHREAD_ONCE_INIT;

static void cwist_gc_pending_key_init(void) {
    pthread_key_create(&g_pending_key, cwist_gc_pending_destroy);
}

/** @brief Lazily create (or return) this thread's pending-sweep list. */
static cwist_gc_pending_t *cwist_gc_pending_get(void) {
    pthread_once(&g_pending_key_once, cwist_gc_pending_key_init);
    cwist_gc_pending_t *pending = (cwist_gc_pending_t *)pthread_getspecific(g_pending_key);
    if (!pending) {
        pending = (cwist_gc_pending_t *)calloc(1, sizeof(*pending));
        if (pending) pthread_setspecific(g_pending_key, pending);
    }
    return pending;
}

void cwist_gc_scope_track(void *ptr) {
    if (!ptr) return;
    cwist_gc_pending_t *pending = cwist_gc_pending_get();
    if (!pending) return;
    if (pending->count == pending->cap) {
        size_t new_cap = pending->cap ? pending->cap * 2 : 8;
        void **grown = (void **)realloc(pending->items, new_cap * sizeof(void *));
        if (!grown) return; /* best-effort: leave ptr untracked rather than fail the alloc */
        pending->items = grown;
        pending->cap = new_cap;
    }
    pending->items[pending->count++] = ptr;
}

bool cwist_gc_scope_untrack(void *ptr) {
    if (!ptr) return false;
    cwist_gc_pending_t *pending = cwist_gc_pending_get();
    if (!pending) return false;
    for (size_t i = 0; i < pending->count; i++) {
        if (pending->items[i] == ptr) {
            pending->items[i] = pending->items[--pending->count];
            return true;
        }
    }
    return false;
}

bool cwist_gc_scope_disown(void *ptr) {
    return cwist_gc_scope_untrack(ptr);
}

void cwist_gc_scope_flush(void) {
    cwist_gc_pending_flush(cwist_gc_pending_get());
}

size_t cwist_gc_scope_pending_count(void) {
    cwist_gc_pending_t *pending = cwist_gc_pending_get();
    return pending ? pending->count : 0;
}
