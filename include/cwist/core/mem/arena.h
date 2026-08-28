/**
 * @file arena.h
 * @brief Request-scoped arena allocator backed by libttak arena generations.
 *
 * Wraps ttak_arena_env_t / ttak_arena_generation_t so a request/response
 * cycle can bump-allocate its fixed-size structures from a single generation
 * buffer and release them all in one shot instead of freeing each node.
 */

#ifndef __CWIST_CORE_MEM_ARENA_H__
#define __CWIST_CORE_MEM_ARENA_H__

#include <stdbool.h>
#include <stddef.h>

/** @brief Default capacity of one arena generation buffer.
 *  A full HTTP/1.1 request+response pair (headers, query map, bodies,
 *  default security headers) bump-allocates ~3.5KB; spills fall back to
 *  the heap, so this only needs to cover the common case. */
#define CWIST_ARENA_DEFAULT_GENERATION_BYTES (8 * 1024)

typedef struct cwist_arena cwist_arena_t;

/**
 * @brief Create an arena backed by a recycled thread-local generation buffer.
 * @param generation_bytes Capacity of the generation buffer (0 = default).
 * @return New arena, or NULL on failure (callers should fall back to heap).
 * @note Default-sized buffers come from a per-thread recycle cache, so
 *       steady-state creation triggers no libttak GC/mem-tree activity.
 */
cwist_arena_t *cwist_arena_create(size_t generation_bytes);

/**
 * @brief Bump-allocate @p size bytes from the active generation.
 * @return Aligned chunk, or NULL when the generation is exhausted
 *         (callers should fall back to cwist_alloc).
 * @note The returned memory is NOT zeroed.
 */
void *cwist_arena_alloc(cwist_arena_t *arena, size_t size);

/**
 * @brief Check whether @p ptr lies inside the active generation buffer.
 */
bool cwist_arena_owns(const cwist_arena_t *arena, const void *ptr);

/**
 * @brief Retire the generation and destroy the arena, releasing every
 *        allocation made from it at once.
 */
void cwist_arena_destroy(cwist_arena_t *arena);

#endif /* __CWIST_CORE_MEM_ARENA_H__ */
