/**
 * @file test_arena.c
 * @brief Unit tests for arena allocator including overflow protection and boundaries.
 */

#include <cwist/core/mem/arena.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

static void test_arena_basic_and_ownership(void) {
    printf("test_arena_basic_and_ownership...\n");
    cwist_arena_t *arena = cwist_arena_create(1024);
    assert(arena != NULL);

    void *p1 = cwist_arena_alloc(arena, 32);
    assert(p1 != NULL);
    assert(cwist_arena_owns(arena, p1));
    assert(((uintptr_t)p1 % 16) == 0);

    void *p2 = cwist_arena_alloc(arena, 64);
    assert(p2 != NULL);
    assert(cwist_arena_owns(arena, p2));
    assert(((uintptr_t)p2 % 16) == 0);
    assert((char *)p2 >= (char *)p1 + 32);

    char external_buf[64];
    assert(!cwist_arena_owns(arena, external_buf));
    assert(!cwist_arena_owns(arena, NULL));
    assert(!cwist_arena_owns(NULL, p1));

    cwist_arena_destroy(arena);
}

static void test_arena_overflow_and_exhaustion(void) {
    printf("test_arena_overflow_and_exhaustion...\n");
    cwist_arena_t *arena = cwist_arena_create(512);
    assert(arena != NULL);

    // Integer overflow attempts near SIZE_MAX should return NULL safely
    assert(cwist_arena_alloc(arena, SIZE_MAX) == NULL);
    assert(cwist_arena_alloc(arena, SIZE_MAX - 5) == NULL);
    assert(cwist_arena_alloc(arena, SIZE_MAX - 14) == NULL);

    // Allocating 0 bytes or NULL arena
    assert(cwist_arena_alloc(arena, 0) == NULL);
    assert(cwist_arena_alloc(NULL, 10) == NULL);

    // Allocation larger than capacity
    assert(cwist_arena_alloc(arena, 1024) == NULL);

    // Valid allocation should still succeed after failed attempts
    void *p = cwist_arena_alloc(arena, 128);
    assert(p != NULL);
    assert(cwist_arena_owns(arena, p));

    cwist_arena_destroy(arena);
}

int main(void) {
    test_arena_basic_and_ownership();
    test_arena_overflow_and_exhaustion();
    printf("All test_arena tests passed!\n");
    return 0;
}
