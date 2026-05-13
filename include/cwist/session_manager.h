/** @file session_manager.h
 * @brief session_manager.h interface.
 */
#ifndef cwist_session_manager_h
#define cwist_session_manager_h

#include <stddef.h>
#include <stdint.h>

struct session_rc_header {
    uint32_t ref_count;
    void (*destructor)(void *);
};

struct session_arena {
    uint8_t *buffer;
    size_t capacity;
    size_t offset;
};

struct session_manager {
    struct session_arena request_arena;
};

/**
 * @brief Initialize a session arena.
 */
void session_arena_init(struct session_arena *arena, uint8_t *buffer, size_t capacity);

/**
 * @brief Allocate from a session arena.
 */
void *session_arena_alloc(struct session_arena *arena, size_t size);

/**
 * @brief Reset a session arena.
 */
void session_arena_reset(struct session_arena *arena);

/**
 * @brief Initialize reference-counted header.
 */
void session_rc_init(struct session_rc_header *header, void (*destructor)(void *));

/**
 * @brief Allocate shared session memory.
 */
void *session_shared_alloc(size_t payload_size, void (*destructor)(void *));

/**
 * @brief Increment reference count.
 */
void session_shared_inc(void *payload);

/**
 * @brief Decrement reference count and free if zero.
 */
void session_shared_dec(void *payload);

/**
 * @brief Initialize session manager.
 */
void session_manager_init(struct session_manager *manager, uint8_t *buffer, size_t capacity);

/**
 * @brief Reset session manager.
 */
void session_manager_reset(struct session_manager *manager);

#endif
