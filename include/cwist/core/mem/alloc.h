/**
 * @file alloc.h
 * @brief Memory allocation wrappers backed by libttak.
 */

#ifndef __CWIST_CORE_MEM_ALLOC_H__
#define __CWIST_CORE_MEM_ALLOC_H__

#include <stddef.h>
#include <ttak/mem/owner.h>
#include <cwist/core/mem/arena.h>

/**
 * @brief Lazily create (or return) the shared CWIST owner context.
 */
ttak_owner_t *cwist_create_owner(void);

/**
 * @brief Allocate zeroed memory via the CWIST owner context.
 */
void *cwist_malloc(size_t size);

/**
 * @brief Allocate zeroed memory tracked by libttak.
 */
void *cwist_alloc(size_t size);

/**
 * @brief Allocate zeroed memory for count elements.
 */
void *cwist_alloc_array(size_t count, size_t elem_size);

/**
 * @brief Resize an existing allocation (libttak tracked).
 */
void *cwist_realloc(void *ptr, size_t new_size);

/**
 * @brief Duplicate a string using libttak-backed storage.
 */
char *cwist_strdup(const char *src);

/**
 * @brief Duplicate up to n bytes of a string (libttak-backed).
 */
char *cwist_strndup(const char *src, size_t n);

/**
 * @brief Free memory obtained via cwist_alloc/cwist_strdup/etc.
 */
void cwist_free(void *ptr);

/**
 * @brief Variable cleanup attribute for GCC/Clang; no-op on other
 *        compilers (e.g. TinyCC), where the variable then needs an
 *        explicit cwist_free() like today.
 *
 * Mirrors TTAK_ATTRIBUTE_CLEANUP in
 * lib/libttak/include/ttak/types/ttak_compiler.h.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define CWIST_ATTRIBUTE_CLEANUP(func) __attribute__((cleanup(func)))
#else
#  define CWIST_ATTRIBUTE_CLEANUP(func)
#endif

/**
 * @brief cleanup-attribute callback: cwist_free()s the pointer stored at
 *        the address the compiler passes in (i.e. the address of the
 *        variable CWIST_DEFER_FREE/cwist_alloc_scoped() was attached to).
 *        Not meant to be called directly.
 */
static inline void cwist_defer_free_cb(void *pp) {
    void **p = (void **)pp;
    if (*p) {
        cwist_free(*p);
    }
}

/**
 * @def CWIST_DEFER_FREE
 * @brief Attach to a pointer variable's declaration to cwist_free() it
 *        automatically when that variable's enclosing block scope exits,
 *        on every return path (GCC/Clang; no-op on TinyCC).
 *
 * Usage:
 * @code
 *   void *buf CWIST_DEFER_FREE = cwist_alloc(256);
 *   if (something) return; // buf is freed here too
 *   ...
 *   // and freed here, at the end of the block it was declared in
 * @endcode
 *
 * Only safe for a pointer that does not outlive the block it is declared
 * in (i.e. never stored into a struct field, global, or queued job
 * argument that another function/thread/job will read later). A pointer
 * that does escape must not use this -- see cwist_gc_scope_track() /
 * cwist_gc_scope_disown() in cwist/core/mem/gc.h instead, which handle
 * lifetimes that cross those boundaries.
 */
#define CWIST_DEFER_FREE CWIST_ATTRIBUTE_CLEANUP(cwist_defer_free_cb)

/**
 * @def cwist_alloc_scoped(var, cast, size)
 * @brief Declare @p var as a @p cast -typed pointer to a fresh
 *        cwist_alloc() block that is freed automatically when the
 *        enclosing block exits. Equivalent to combining cwist_alloc()
 *        with CWIST_DEFER_FREE, spelled as one declaration.
 *
 * @code
 *   cwist_alloc_scoped(name, char *, 64);
 *   snprintf(name, 64, "...");
 *   // name is cwist_free()'d automatically here
 * @endcode
 */
#define cwist_alloc_scoped(var, cast, size) \
    void *_cwist_scoped_##var CWIST_DEFER_FREE = cwist_alloc(size); \
    cast var = (cast)_cwist_scoped_##var

/**
 * @brief Backing state for cwist_scratch_alloc()/CWIST_SCRATCH_DEFER: a
 *        single block-scoped allocation that prefers a recycled arena
 *        generation and falls back to cwist_alloc() when the request
 *        doesn't fit one (see cwist/core/mem/arena.h -- only
 *        CWIST_ARENA_DEFAULT_GENERATION_BYTES-sized generations hit the
 *        zero-libttak-GC recycle path, so cwist_scratch_alloc() always
 *        requests the default size and never a custom one).
 *
 * Zero-initialize before use (`= {0}`); do not touch fields directly.
 */
typedef struct {
    cwist_arena_t *arena;
    void *ptr;
    int heap; /* 1 if ptr came from cwist_alloc() instead of the arena */
} cwist_scratch_t;

/**
 * @brief Allocate @p size scratch bytes, preferring a recycled arena
 *        generation over cwist_alloc()'s heavier owner-guarded path.
 * @param s Scratch handle; zero-initialize it (`cwist_scratch_t s = {0};`)
 *          before the first call. One handle holds one allocation.
 * @param size Bytes needed.
 * @return Pointer to @p size (NOT zeroed) bytes, or NULL on failure.
 */
static inline void *cwist_scratch_alloc(cwist_scratch_t *s, size_t size) {
    s->arena = cwist_arena_create(0);
    if (s->arena) {
        s->ptr = cwist_arena_alloc(s->arena, size);
        if (s->ptr) {
            s->heap = 0;
            return s->ptr;
        }
    }
    s->ptr = cwist_alloc(size);
    s->heap = 1;
    return s->ptr;
}

/**
 * @brief cleanup-attribute callback for CWIST_SCRATCH_DEFER. Not meant to
 *        be called directly.
 */
static inline void cwist_scratch_cleanup(void *sp) {
    cwist_scratch_t *s = (cwist_scratch_t *)sp;
    if (s->heap && s->ptr) {
        cwist_free(s->ptr);
    }
    if (s->arena) {
        cwist_arena_destroy(s->arena);
    }
}

/**
 * @def CWIST_SCRATCH_DEFER
 * @brief Attach to a cwist_scratch_t variable's declaration so its
 *        allocation (arena- or heap-backed, whichever cwist_scratch_alloc()
 *        picked) is released automatically when the enclosing block exits.
 *
 * @code
 *   cwist_scratch_t buf_s CWIST_SCRATCH_DEFER = {0};
 *   unsigned char *buf = cwist_scratch_alloc(&buf_s, needed);
 *   if (!buf) return -1;
 *   ...
 *   // buf_s's backing storage is released here, on every return path
 * @endcode
 *
 * Same escape rule as CWIST_DEFER_FREE: only for a pointer that does not
 * outlive the block cwist_scratch_t was declared in.
 */
#define CWIST_SCRATCH_DEFER CWIST_ATTRIBUTE_CLEANUP(cwist_scratch_cleanup)

#endif
