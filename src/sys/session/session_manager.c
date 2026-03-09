#include <cwist/sys/session/session_manager.h>
#include <cwist/core/mem/alloc.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file session_manager.c
 * @brief Session allocation helpers built from a resettable arena and RC payloads.
 */

/**
 * @brief Initialize a linear arena over caller-provided storage.
 * @param arena Arena to initialize.
 * @param buffer Backing storage to consume sequentially.
 * @param capacity Total number of bytes available in @p buffer.
 */
void session_arena_init(struct session_arena *arena, uint8_t *buffer, size_t capacity) {
    if (!arena) return;
    arena->buffer = buffer;
    arena->capacity = capacity;
    arena->offset = 0;
}

/**
 * @brief Allocate aligned bytes from the request arena.
 * @param arena Arena that owns the backing storage.
 * @param size Number of bytes requested before alignment.
 * @return Pointer to a contiguous region, or NULL when the arena is exhausted.
 */
void *session_arena_alloc(struct session_arena *arena, size_t size) {
    if (!arena || !arena->buffer) return NULL;
    size = (size + 7u) & ~7u;
    if (arena->offset + size > arena->capacity) {
        return NULL;
    }
    void *ptr = arena->buffer + arena->offset;
    arena->offset += size;
    return ptr;
}

/**
 * @brief Rewind the arena so subsequent allocations reuse the full buffer.
 * @param arena Arena to reset.
 */
void session_arena_reset(struct session_arena *arena) {
    if (!arena) return;
    arena->offset = 0;
}

/**
 * @brief Initialize the reference-count metadata for a shared payload.
 * @param header Header that precedes the payload.
 * @param destructor Optional callback run just before the payload is freed.
 */
void session_rc_init(struct session_rc_header *header, void (*destructor)(void *)) {
    if (!header) return;
    header->ref_count = 1;
    header->destructor = destructor;
}

/**
 * @brief Allocate a shared payload with an in-band reference-count header.
 * @param payload_size Number of bytes visible to callers.
 * @param destructor Optional callback invoked before memory is released.
 * @return Zeroed payload pointer, or NULL when allocation fails.
 */
void *session_shared_alloc(size_t payload_size, void (*destructor)(void *)) {
    size_t total = sizeof(struct session_rc_header) + payload_size;
    uint8_t *raw = (uint8_t *)cwist_alloc(total);
    if (!raw) return NULL;
    struct session_rc_header *header = (struct session_rc_header *)raw;
    session_rc_init(header, destructor);
    void *payload = raw + sizeof(struct session_rc_header);
    memset(payload, 0, payload_size);
    return payload;
}

/**
 * @brief Increment the reference count for a shared payload.
 * @param payload Payload returned by session_shared_alloc().
 */
void session_shared_inc(void *payload) {
    if (!payload) return;
    struct session_rc_header *header = (struct session_rc_header *)((uint8_t *)payload - sizeof(struct session_rc_header));
    header->ref_count += 1;
}

/**
 * @brief Drop a reference and destroy the payload when the count reaches zero.
 * @param payload Payload returned by session_shared_alloc().
 */
void session_shared_dec(void *payload) {
    if (!payload) return;
    struct session_rc_header *header = (struct session_rc_header *)((uint8_t *)payload - sizeof(struct session_rc_header));
    if (header->ref_count == 0) return;
    header->ref_count -= 1;
    if (header->ref_count == 0) {
        if (header->destructor) {
            header->destructor(payload);
        }
        cwist_free(header);
    }
}

/**
 * @brief Initialize the request-scoped allocator state for a session manager.
 * @param manager Manager to initialize.
 * @param buffer Backing arena storage.
 * @param capacity Size of @p buffer in bytes.
 */
void session_manager_init(struct session_manager *manager, uint8_t *buffer, size_t capacity) {
    if (!manager) return;
    session_arena_init(&manager->request_arena, buffer, capacity);
}

/**
 * @brief Reset all request-scoped allocations owned by the manager.
 * @param manager Manager whose arena should be rewound.
 */
void session_manager_reset(struct session_manager *manager) {
    if (!manager) return;
    session_arena_reset(&manager->request_arena);
}
