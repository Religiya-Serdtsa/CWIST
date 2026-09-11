/**
 * @file alloc.h
 * @brief Memory allocation wrappers backed by libttak.
 */

#ifndef __CWIST_CORE_MEM_ALLOC_H__
#define __CWIST_CORE_MEM_ALLOC_H__

#include <stddef.h>
#include <ttak/mem/owner.h>

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

#endif
