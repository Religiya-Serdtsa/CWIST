/**
 * @file arena.c
 * @brief Request-scoped arena allocator built on libttak arena generations.
 *
 * Performance notes:
 * - Generation buffers are recycled through a per-thread free list, so the
 *   steady-state request cycle performs no libttak mem-tree registration or
 *   epoch-GC work at all; ttak_mem_alloc/free is only touched when the cache
 *   underflows/overflows (thread start or concurrency watermark changes).
 * - The ttak_arena_env_t (epoch/GC coordinator) lives in thread-local storage
 *   and is initialized once per thread, never per request.
 * - Cleanup on thread exit is handled through a pthread key destructor, which
 *   keeps this file portable C11/POSIX with no architecture-specific code.
 */

#include <cwist/core/mem/arena.h>

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include <ttak/mem/arena_helper.h>
#include <ttak/mem/mem.h>
#include <ttak/timing/timing.h>

/** @brief Maximum recycled generation buffers retained per thread. */
#define CWIST_ARENA_CACHE_MAX 8

struct cwist_arena {
    ttak_arena_generation_t gen;   /**< Active generation descriptor. */
    size_t generation_bytes;       /**< Buffer capacity (cache key). */
    pthread_t owner_tid;           /**< Thread that allocated the arena. */
};

/**
 * @brief Per-thread arena state: shared env plus the recycled-buffer cache.
 */
typedef struct cwist_arena_tls {
    ttak_arena_env_t env;          /**< Long-lived generation coordinator. */
    void *bufs[CWIST_ARENA_CACHE_MAX]; /**< Recycled generation buffers. */
    size_t buf_count;              /**< Number of entries in @ref bufs. */
    cwist_arena_t *structs[CWIST_ARENA_CACHE_MAX]; /**< Recycled arena structs. */
    size_t struct_count;           /**< Number of entries in @ref structs. */
    uint32_t epoch_seq;            /**< Epoch seed rotating per request. */
} cwist_arena_tls_t;

#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L)
static _Thread_local cwist_arena_tls_t *t_arena_tls = NULL;
#elif defined(__GNUC__) || defined(__clang__)
static __thread cwist_arena_tls_t *t_arena_tls = NULL;
#endif

static pthread_key_t g_arena_tls_key;
static pthread_once_t g_arena_tls_once = PTHREAD_ONCE_INIT;

/**
 * @brief Release every cached buffer and the env when a thread exits.
 * @param ptr Thread-local arena state allocated by cwist_arena_tls_get().
 */
static void cwist_arena_tls_destroy(void *ptr) {
    cwist_arena_tls_t *tls = (cwist_arena_tls_t *)ptr;
    if (!tls) return;
#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L) || defined(__GNUC__) || defined(__clang__)
    t_arena_tls = NULL;
#endif
    for (size_t i = 0; i < tls->buf_count; i++) {
        ttak_mem_free(tls->bufs[i]);
    }
    for (size_t i = 0; i < tls->struct_count; i++) {
        free(tls->structs[i]);
    }
    ttak_arena_env_destroy(&tls->env);
    free(tls);
}

static void cwist_arena_tls_key_init(void) {
    pthread_key_create(&g_arena_tls_key, cwist_arena_tls_destroy);
}

/**
 * @brief Fetch (lazily creating) this thread's arena state.
 * @return Thread-local state, or NULL on allocation/setup failure.
 */
static inline cwist_arena_tls_t *cwist_arena_tls_get(void) {
#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L) || defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect(t_arena_tls != NULL, 1)) {
        return t_arena_tls;
    }
#endif

    if (pthread_once(&g_arena_tls_once, cwist_arena_tls_key_init) != 0) {
        return NULL;
    }
    cwist_arena_tls_t *tls = (cwist_arena_tls_t *)pthread_getspecific(g_arena_tls_key);
    if (tls) {
#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L) || defined(__GNUC__) || defined(__clang__)
        t_arena_tls = tls;
#endif
        return tls;
    }

    tls = (cwist_arena_tls_t *)calloc(1, sizeof(cwist_arena_tls_t));
    if (!tls) return NULL;

    ttak_arena_env_config_t config;
    ttak_arena_env_config_init(&config);
    config.generation_bytes = CWIST_ARENA_DEFAULT_GENERATION_BYTES;
    if (!ttak_arena_env_init(&tls->env, &config)) {
        free(tls);
        return NULL;
    }
    if (pthread_setspecific(g_arena_tls_key, tls) != 0) {
        ttak_arena_env_destroy(&tls->env);
        free(tls);
        return NULL;
    }
#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L) || defined(__GNUC__) || defined(__clang__)
    t_arena_tls = tls;
#endif
    return tls;
}

cwist_arena_t *cwist_arena_create(size_t generation_bytes) {
    cwist_arena_tls_t *tls = cwist_arena_tls_get();
    if (!tls) return NULL;

    size_t bytes = generation_bytes ? generation_bytes
                                    : CWIST_ARENA_DEFAULT_GENERATION_BYTES;

    /* Only default-sized buffers participate in the recycle cache. */
    void *buffer = NULL;
    if (bytes == CWIST_ARENA_DEFAULT_GENERATION_BYTES && tls->buf_count > 0) {
        buffer = tls->bufs[--tls->buf_count];
    }
    if (!buffer) {
        buffer = ttak_mem_alloc_with_flags_raw(bytes,
                                               __TTAK_UNSAFE_MEM_FOREVER__,
                                               ttak_get_tick_count(),
                                               tls->env.config.alloc_flags);
        if (!buffer) return NULL;
    }

    cwist_arena_t *arena = tls->struct_count > 0
        ? tls->structs[--tls->struct_count]
        : (cwist_arena_t *)calloc(1, sizeof(cwist_arena_t));
    if (!arena) {
        if (bytes == CWIST_ARENA_DEFAULT_GENERATION_BYTES &&
            tls->buf_count < CWIST_ARENA_CACHE_MAX) {
            tls->bufs[tls->buf_count++] = buffer;
        } else {
            ttak_mem_free(buffer);
        }
        return NULL;
    }

    arena->gen.base = buffer;
    arena->gen.capacity = bytes;
    arena->gen.used = 0;
    arena->gen.epoch_id = ++tls->epoch_seq;
    arena->generation_bytes = bytes;
    arena->owner_tid = pthread_self();
    return arena;
}

void *cwist_arena_alloc(cwist_arena_t *arena, size_t size) {
    if (!arena || !size || !arena->gen.base) return NULL;
    /* Plain 16-byte-aligned bump over the generation buffer.  Skipping
     * ttak_arena_generation_claim avoids its per-claim cache-line padding
     * and scatter offset; exhaustion still returns NULL so callers fall
     * back to the heap exactly as before. */
    size = (size + 15u) & ~15u;
    if (size > arena->gen.capacity - arena->gen.used) return NULL;
    void *ptr = (uint8_t *)arena->gen.base + arena->gen.used;
    arena->gen.used += size;
    return ptr;
}

bool cwist_arena_owns(const cwist_arena_t *arena, const void *ptr) {
    if (!arena || !ptr || !arena->gen.base) return false;
    const uint8_t *p = (const uint8_t *)ptr;
    const uint8_t *base = (const uint8_t *)arena->gen.base;
    return p >= base && p < base + arena->gen.capacity;
}

void cwist_arena_destroy(cwist_arena_t *arena) {
    if (!arena) return;
    void *buffer = arena->gen.base;
    size_t bytes = arena->generation_bytes;
    bool default_sized = (bytes == CWIST_ARENA_DEFAULT_GENERATION_BYTES);

    /* If destroyed on a foreign thread (e.g. cross-thread async completion),
     * release directly to avoid polluting foreign thread-local caches. */
    bool is_owner = pthread_equal(arena->owner_tid, pthread_self());
    cwist_arena_tls_t *tls = is_owner ? cwist_arena_tls_get() : NULL;

    if (tls && tls->struct_count < CWIST_ARENA_CACHE_MAX) {
        /* Recycle the arena struct itself along with the buffer. */
        tls->structs[tls->struct_count++] = arena;
    } else {
        free(arena);
    }

    if (!buffer) return;
    if (tls && default_sized &&
        tls->buf_count < CWIST_ARENA_CACHE_MAX) {
        /* Recycle: the next request reuses this buffer with zero GC work. */
        tls->bufs[tls->buf_count++] = buffer;
    } else {
        ttak_mem_free(buffer);
    }
}
