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

void session_arena_init(struct session_arena *arena, uint8_t *buffer, size_t capacity);
void *session_arena_alloc(struct session_arena *arena, size_t size);
void session_arena_reset(struct session_arena *arena);

void session_rc_init(struct session_rc_header *header, void (*destructor)(void *));
void *session_shared_alloc(size_t payload_size, void (*destructor)(void *));
void session_shared_inc(void *payload);
void session_shared_dec(void *payload);

void session_manager_init(struct session_manager *manager, uint8_t *buffer, size_t capacity);
void session_manager_reset(struct session_manager *manager);

#endif
