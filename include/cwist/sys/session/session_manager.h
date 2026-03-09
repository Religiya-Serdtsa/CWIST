#ifndef cwist_session_manager_h
#define cwist_session_manager_h

/**
 * @file session_manager.h
 * @brief Request-scoped arena and shared-payload helpers for session data.
 */

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Reference-count header placed immediately before shared session payloads.
 */
struct session_rc_header {
    uint32_t ref_count;
    void (*destructor)(void *);
};

/**
 * @brief Linear arena used for fast request-lifetime allocations.
 */
struct session_arena {
    uint8_t *buffer;
    size_t capacity;
    size_t offset;
};

/**
 * @brief Top-level session allocator state owned by a request context.
 */
struct session_manager {
    struct session_arena request_arena;
};

/**
 * @brief Bind a caller-owned buffer to a session arena and reset its cursor.
 * @param arena Arena to initialize.
 * @param buffer Backing storage owned by the caller.
 * @param capacity Size of @p buffer in bytes.
 */
void session_arena_init(struct session_arena *arena, uint8_t *buffer, size_t capacity);
/**
 * @brief Allocate aligned storage from the request arena.
 * @param arena Arena to allocate from.
 * @param size Requested payload size in bytes.
 * @return Pointer to the reserved range, or NULL when capacity is exhausted.
 */
void *session_arena_alloc(struct session_arena *arena, size_t size);
/**
 * @brief Reset the request arena so future allocations reuse the full buffer.
 * @param arena Arena to reset.
 */
void session_arena_reset(struct session_arena *arena);

/**
 * @brief Initialize the metadata that tracks a shared session allocation.
 * @param header Header to initialize.
 * @param destructor Optional destructor invoked before the payload is freed.
 */
void session_rc_init(struct session_rc_header *header, void (*destructor)(void *));
/**
 * @brief Allocate a zeroed payload preceded by a reference-count header.
 * @param payload_size Number of bytes to expose to the caller.
 * @param destructor Optional destructor for the payload.
 * @return Pointer to the payload region, or NULL when allocation fails.
 */
void *session_shared_alloc(size_t payload_size, void (*destructor)(void *));
/**
 * @brief Increment the reference count for a shared payload.
 * @param payload Payload returned by session_shared_alloc().
 */
void session_shared_inc(void *payload);
/**
 * @brief Decrement the reference count and free the payload when it reaches zero.
 * @param payload Payload returned by session_shared_alloc().
 */
void session_shared_dec(void *payload);

/**
 * @brief Initialize a session manager around a caller-supplied arena buffer.
 * @param manager Session manager to initialize.
 * @param buffer Buffer used for request-scoped arena allocations.
 * @param capacity Size of @p buffer in bytes.
 */
void session_manager_init(struct session_manager *manager, uint8_t *buffer, size_t capacity);
/**
 * @brief Reset all request-scoped allocations owned by the manager.
 * @param manager Session manager to reset.
 */
void session_manager_reset(struct session_manager *manager);

#endif
