#include <cwist/sys/session/session_manager.h>
#include <cwist/core/mem/alloc.h>
#include <stdlib.h>
#include <string.h>

void session_arena_init(struct session_arena *arena, uint8_t *buffer, size_t capacity) {
    if (!arena) return;
    arena->buffer = buffer;
    arena->capacity = capacity;
    arena->offset = 0;
}

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

void session_arena_reset(struct session_arena *arena) {
    if (!arena) return;
    arena->offset = 0;
}

void session_rc_init(struct session_rc_header *header, void (*destructor)(void *)) {
    if (!header) return;
    header->ref_count = 1;
    header->destructor = destructor;
}

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

void session_shared_inc(void *payload) {
    if (!payload) return;
    struct session_rc_header *header = (struct session_rc_header *)((uint8_t *)payload - sizeof(struct session_rc_header));
    header->ref_count += 1;
}

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

void session_manager_init(struct session_manager *manager, uint8_t *buffer, size_t capacity) {
    if (!manager) return;
    session_arena_init(&manager->request_arena, buffer, capacity);
}

void session_manager_reset(struct session_manager *manager) {
    if (!manager) return;
    session_arena_reset(&manager->request_arena);
}
