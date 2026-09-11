/**
 * @file gc.h
 * @brief Epoch-based garbage collection wrapper.
 */

#ifndef __CWIST_CORE_MEM_GC_H__
#define __CWIST_CORE_MEM_GC_H__

#include <stdbool.h>
#include <stddef.h>
#include <ttak/compat/stdatomic.h>
#include <ttak/mem/epoch_gc.h>

/**
 * @brief High-level wrapper over libttak's epoch GC.
 *
 * These helpers keep initialization/destruction scoped to CWIST so
 * applications can toggle GC behavior without touching libttak internals.
 */
typedef struct cwist_gc {
    ttak_epoch_gc_t impl;
    /// Check if CWIST GC has initialized
    bool initialized;
    /// Check if GC should scan and reclaim objects
    _Atomic bool auto_rotated;
} cwist_gc_t;

/**
 * @brief Initialize (if needed) and configure the GC context.
 *
 * @param gc Target context.
 * @param manual_rotation When true, force manual rotations instead of the background thread.
 */
void cwist_gc(cwist_gc_t *gc, bool manual_rotation);

/**
 * @brief Shut down and free internal GC state.
 */
void cwist_gc_shutdown(cwist_gc_t *gc);

/**
 * @brief Rotate the GC epoch and reclaim retired nodes.
 */
void cwist_gc_rotate(cwist_gc_t *gc);

/**
 * @brief Toggle whether the GC context reclaims automatically.
 *
 * @param gc Target context.
 * @param enabled When true, mark the context as auto-rotated (full-GC mode).
 */
void cwist_gc_auto_rotate(cwist_gc_t *gc, bool enabled);

/**
 * @brief Check whether the GC context is currently set to auto-rotate.
 */
bool cwist_gc_auto_rotated(cwist_gc_t *gc);

/**
 * @brief Register a pointer without specifying a size (defaults to 0).
 */
void cwist_reg_ptr(cwist_gc_t *gc, void *ptr);

/**
 * @brief Register a pointer and its size for better accounting.
 */
void cwist_reg_ptr_sized(cwist_gc_t *gc, void *ptr, size_t size);

/**
 * @brief Obtain the raw libttak epoch GC pointer (advanced usage).
 */
ttak_epoch_gc_t *cwist_gc_raw(cwist_gc_t *gc);

/**
 * @brief Allocate a block meant to be released through cwist_ebr_free().
 *
 * Identical to cwist_alloc() today: a freshly allocated block is not yet
 * shared, so nothing needs epoch protection until it is published to other
 * threads. The distinct name exists to mark call sites that pair with
 * cwist_ebr_free() instead of cwist_free().
 *
 * @param size Number of bytes to allocate.
 * @return Newly allocated block, or NULL on failure.
 */
void *cwist_ebr_alloc(size_t size);

/**
 * @brief Defer release of a block shared across threads via epoch reclamation.
 *
 * Unlike cwist_free(), this does not release @p ptr immediately. It hands
 * the pointer to libttak's EBR (ttak_epoch_retire) so the actual free only
 * runs once every thread that might still be inside a ttak_epoch_enter() /
 * ttak_epoch_exit() critical section touching @p ptr has left it.
 *
 * Callers on the reading side MUST wrap their access to the shared pointer
 * in ttak_epoch_enter()/ttak_epoch_exit() for this guarantee to hold; this
 * function alone does not protect readers that skip that discipline.
 *
 * The actual free only happens once something calls ttak_epoch_reclaim()
 * (see cwist_gc_pipeline_tick()) and observes it is safe.
 *
 * @param ptr Block previously returned by cwist_ebr_alloc() (or cwist_alloc()).
 */
void cwist_ebr_free(void *ptr);

/**
 * @brief Drive the EBR pipeline: attempt to advance the epoch and run any
 *        cwist_ebr_free() callbacks that are now safe to execute.
 *
 * Call this periodically (e.g. once per event-loop tick) from a single
 * well-known place; nothing retired via cwist_ebr_free() is ever actually
 * released unless this runs.
 */
void cwist_gc_pipeline_tick(void);

/**
 * @brief Enable or disable full-GC mode process-wide.
 *
 * Hardened, boot-time-only setter: the first call wins and is latched in;
 * every later call -- from any thread, deliberate re-invocation or
 * otherwise -- is a silent no-op, and the backing storage is mprotect()'d
 * read-only after that first call so a raw memory-corruption write can't
 * flip it either. See gc.c's cwist_full_gc_guard_t for the full threat
 * model this defends against.
 *
 * @param enable When true, cwist_alloc() starts registering blocks with the
 *               per-thread pending-sweep list and cwist_free() starts
 *               unregistering them; a worker thread that exits without
 *               freeing everything it allocated has the remainder swept via
 *               cwist_ebr_free() automatically.
 */
void cwist_full_gc(bool enable);

/**
 * @brief Check whether full-GC mode is currently enabled.
 */
bool cwist_full_gc_enabled(void);

/**
 * @brief Check whether the full-GC toggle has been latched (i.e.
 *        cwist_full_gc() has been called once already and every further
 *        call will be ignored). Diagnostic/testing use.
 */
bool cwist_full_gc_locked(void);

/**
 * @brief Raw pointer to the full-GC toggle's guard page, or NULL if the
 *        mmap() backing it failed at process startup. Advanced/testing use
 *        only -- e.g. to confirm the page is actually read-only after
 *        cwist_full_gc() has been called once.
 */
void *cwist_full_gc_guard_page(void);

/**
 * @brief Register a cwist_alloc() block with the current thread's
 *        pending-sweep list (full-GC mode only).
 *
 * Called by cwist_alloc() itself; not meant to be called directly.
 */
void cwist_gc_scope_track(void *ptr);

/**
 * @brief Remove a block from the current thread's pending-sweep list.
 *
 * Called by cwist_free() itself before it releases @p ptr, so a block that
 * was freed explicitly is never swept a second time at thread exit.
 *
 * @return true if @p ptr was pending (and has been removed); false if it
 *         was never tracked (e.g. it came from cwist_strdup()/cwist_realloc()
 *         instead of cwist_alloc(), or full-GC mode was off at alloc time).
 *         Either way the caller should proceed to free @p ptr normally.
 */
bool cwist_gc_scope_untrack(void *ptr);

/**
 * @brief Remove @p ptr from the current thread's pending-sweep list without
 *        freeing it, because ownership is being handed off to other code
 *        that will call cwist_free() (or cwist_ebr_free()) on it later,
 *        possibly from a different thread.
 *
 * Without this call, a block allocated by one unit of work (e.g. a queued
 * job) and stored somewhere for a later unit of work to release would be
 * auto-swept as soon as the allocating unit of work finishes -- the sweep
 * logic cannot otherwise tell "nobody will ever free this" apart from
 * "someone else will free this later". Call cwist_gc_scope_disown() at the
 * point of handoff, before the pointer becomes visible to whatever will
 * eventually free it.
 *
 * This is exactly cwist_gc_scope_untrack() under a name that documents the
 * handoff use case; the two are interchangeable.
 *
 * @return true if @p ptr was pending and has been removed; false if it was
 *         never tracked (full-GC mode was off at alloc time, or it was
 *         already disowned/freed).
 */
bool cwist_gc_scope_disown(void *ptr);

/**
 * @brief Retire every block still on the current thread's pending-sweep
 *        list right now, instead of waiting for the thread to exit.
 *
 * Call this at the end of each unit of work a worker thread processes (a
 * request, a queued job) so a forgotten cwist_free() is caught immediately
 * rather than accumulating for the lifetime of a long-lived worker thread.
 * The actual free still only happens once something calls
 * cwist_gc_pipeline_tick() / ttak_epoch_reclaim() afterwards.
 */
void cwist_gc_scope_flush(void);

/**
 * @brief Number of blocks currently on the calling thread's pending-sweep
 *        list. Introspection/testing helper.
 */
size_t cwist_gc_scope_pending_count(void);

/**
 * @brief One-shot guard ensuring a shared teardown path runs exactly once.
 *
 * Use this when the same release path (e.g. detaching two sibling
 * references and freeing what they both point to) may be entered
 * concurrently from more than one thread, and exactly one of them must
 * win. Initialize with cwist_release_guard_init(); every racer then calls
 * cwist_release_guard_acquire() and only the caller that gets `true` back
 * performs the detach + free.
 */
typedef atomic_bool cwist_release_guard_t;

/**
 * @brief Initialize a release guard to its unclaimed state.
 */
void cwist_release_guard_init(cwist_release_guard_t *guard);

/**
 * @brief Attempt to claim the guard.
 * @return true exactly once across all racing callers; false to every
 *         other caller (including ones arriving after the winner finishes).
 */
bool cwist_release_guard_acquire(cwist_release_guard_t *guard);

#endif
