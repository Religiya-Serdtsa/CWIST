#ifndef __CWIST_CORE_MEM_GC_H__
#define __CWIST_CORE_MEM_GC_H__

#include <stdbool.h>
#include <stddef.h>
#include <ttak/mem/epoch_gc.h>

/**
 * @brief High-level wrapper over libttak's epoch GC.
 *
 * These helpers keep initialization/destruction scoped to CWIST so
 * applications can toggle GC behavior without touching libttak internals.
 */
typedef struct cwist_gc {
    ttak_epoch_gc_t impl;
    bool initialized;
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

#endif
