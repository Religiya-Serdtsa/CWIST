#include <cwist/core/mem/alloc.h>
#include <ttak/mem/mem.h>
#include <ttak/timing/timing.h>
#include <ttak/sync/sync.h>
#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>
#include <stdatomic.h>

/**
 * @file alloc.c
 * @brief Memory allocation bridge that can route CWIST allocations through libttak owner guards.
 */

#ifndef ATOMIC_VAR_INIT
#define ATOMIC_VAR_INIT(value) (value)
#endif

#define CWIST_OWNER_RESOURCE "cwist_mem_resource"
#define CWIST_OWNER_ALLOC_FUNC "cwist_mem_alloc"
#define CWIST_OWNER_FREE_FUNC "cwist_mem_free"
#define CWIST_OWNER_REALLOC_FUNC "cwist_mem_realloc"

/**
 * @brief Read the current tick counter used by libttak allocation APIs.
 * @return Monotonic tick value suitable for libttak timestamps.
 */
static inline uint64_t cwist_mem_now(void) {
    return ttak_get_tick_count();
}

typedef struct {
    ttak_mem_flags_t flags;
} cwist_owner_policy_t;

typedef struct {
    size_t size;
    void *result;
} cwist_owner_alloc_args_t;

typedef struct {
    void *ptr;
} cwist_owner_free_args_t;

typedef struct {
    void *ptr;
    size_t size;
    void *result;
} cwist_owner_realloc_args_t;

static ttak_mutex_t g_owner_lock;
#if defined(__TINYC__)
static atomic_bool g_owner_lock_ready = false;
#else
static atomic_bool g_owner_lock_ready = ATOMIC_VAR_INIT(false);
#endif
static ttak_owner_t *g_owner = NULL;
static cwist_owner_policy_t g_owner_policy = {
    .flags = TTAK_MEM_DEFAULT | TTAK_MEM_STRICT_CHECK
};
static bool g_owner_enabled = false;

/**
 * @brief Lazily initialize the mutex protecting the shared owner context.
 */
static void cwist_owner_lock_init(void) {
    if (atomic_load_explicit(&g_owner_lock_ready, memory_order_acquire)) {
        return;
    }
    static atomic_flag init_flag = ATOMIC_FLAG_INIT;
    while (atomic_flag_test_and_set_explicit(&init_flag, memory_order_acquire)) {
        // Spin until initialization completes; extremely rare path.
    }
    if (!atomic_load_explicit(&g_owner_lock_ready, memory_order_relaxed)) {
        ttak_mutex_init(&g_owner_lock);
        atomic_store_explicit(&g_owner_lock_ready, true, memory_order_release);
    }
    atomic_flag_clear_explicit(&init_flag, memory_order_release);
}

/**
 * @brief Owner callback that performs guarded allocation through libttak.
 * @param ctx Owner policy resource registered with libttak.
 * @param args Allocation request/result bundle.
 */
static void cwist_owner_alloc(void *ctx, void *args) {
    cwist_owner_policy_t *policy = (cwist_owner_policy_t *)ctx;
    cwist_owner_alloc_args_t *req = (cwist_owner_alloc_args_t *)args;
    if (!req) return;
    size_t actual = req->size ? req->size : 1;
    ttak_mem_flags_t flags = policy ? policy->flags : TTAK_MEM_DEFAULT;
    req->result = ttak_mem_alloc_safe(actual,
                                      __TTAK_UNSAFE_MEM_FOREVER__,
                                      cwist_mem_now(),
                                      false,
                                      false,
                                      true,
                                      true,
                                      flags);
}

/**
 * @brief Owner callback that frees guarded allocations through libttak.
 * @param ctx Unused callback context.
 * @param args Free request bundle containing the pointer to release.
 */
static void cwist_owner_free(void *ctx, void *args) {
    (void)ctx;
    cwist_owner_free_args_t *req = (cwist_owner_free_args_t *)args;
    if (!req || !req->ptr) return;
    ttak_mem_free(req->ptr);
    req->ptr = NULL;
}

/**
 * @brief Owner callback that resizes guarded allocations through libttak.
 * @param ctx Owner policy resource registered with libttak.
 * @param args Reallocation request/result bundle.
 */
static void cwist_owner_realloc(void *ctx, void *args) {
    cwist_owner_policy_t *policy = (cwist_owner_policy_t *)ctx;
    cwist_owner_realloc_args_t *req = (cwist_owner_realloc_args_t *)args;
    if (!req) return;
    size_t actual = req->size ? req->size : 1;
    ttak_mem_flags_t flags = policy ? policy->flags : TTAK_MEM_DEFAULT;
    req->result = ttak_mem_realloc_safe(req->ptr,
                                        actual,
                                        __TTAK_UNSAFE_MEM_FOREVER__,
                                        cwist_mem_now(),
                                        true,
                                        flags);
}

/**
 * @brief Create or return the shared owner context used for guarded memory operations.
 * @return Shared libttak owner context, or NULL when initialization fails.
 */
ttak_owner_t *cwist_create_owner(void) {
    cwist_owner_lock_init();
    ttak_mutex_lock(&g_owner_lock);
    if (!g_owner) {
        ttak_owner_t *owner = ttak_owner_create(TTAK_OWNER_SAFE_DEFAULT | TTAK_OWNER_DENY_DANGEROUS_MEM);
        if (owner) {
            bool ok = true;
            ok &= ttak_owner_register_resource(owner, CWIST_OWNER_RESOURCE, &g_owner_policy);
            ok &= ttak_owner_register_func(owner, CWIST_OWNER_ALLOC_FUNC, cwist_owner_alloc);
            ok &= ttak_owner_register_func(owner, CWIST_OWNER_FREE_FUNC, cwist_owner_free);
            ok &= ttak_owner_register_func(owner, CWIST_OWNER_REALLOC_FUNC, cwist_owner_realloc);
            if (!ok) {
                ttak_owner_destroy(owner);
                owner = NULL;
                fprintf(stderr, "[CWIST] Failed to register owner guards\n");
            }
        } else {
            fprintf(stderr, "[CWIST] Failed to create owner context\n");
        }
        g_owner = owner;
    }
    ttak_owner_t *owner = g_owner;
    ttak_mutex_unlock(&g_owner_lock);
    return owner;
}

/**
 * @brief Execute a registered owner function against the shared CWIST resource.
 * @param func_name Registered owner function name to invoke.
 * @param args Opaque request/result structure forwarded to libttak.
 * @return true when libttak accepted and executed the request.
 */
static bool cwist_owner_call(const char *func_name, void *args) {
    ttak_owner_t *owner = cwist_create_owner();
    if (!owner) {
        fprintf(stderr, "[CWIST] Owner unavailable for %s\n", func_name);
        return false;
    }
    if (!ttak_owner_execute(owner, func_name, CWIST_OWNER_RESOURCE, args)) {
        fprintf(stderr, "[CWIST] Owner rejected call: %s\n", func_name);
        return false;
    }
    return true;
}

/**
 * @brief Abort the process when guarded memory semantics cannot be preserved.
 * @param func_name Function name that could not be executed safely.
 */
static void cwist_owner_abort(const char *func_name) {
    fprintf(stderr, "[CWIST] Unable to honor %s; aborting to prevent unsafe memory access\n", func_name);
    abort();
}

/**
 * @brief Allocate zeroed memory either via calloc or the libttak owner context.
 * @param size Requested allocation size in bytes.
 * @return Zeroed memory block, or NULL when allocation fails.
 */
void *cwist_malloc(size_t size) {
    size_t actual = size ? size : 1;
    if (!g_owner_enabled) {
        void *ptr = calloc(1, actual);
        return ptr;
    }
    cwist_owner_alloc_args_t args = {
        .size = actual,
        .result = NULL
    };
    if (!cwist_owner_call(CWIST_OWNER_ALLOC_FUNC, &args)) {
        return NULL;
    }
    return args.result;
}

/**
 * @brief Alias of cwist_malloc for CWIST's preferred naming.
 * @param size Requested allocation size in bytes.
 * @return Zeroed memory block, or NULL when allocation fails.
 */
void *cwist_alloc(size_t size) {
    return cwist_malloc(size);
}

/**
 * @brief Allocate zeroed storage for a fixed number of elements.
 * @param count Number of elements requested.
 * @param elem_size Size of each element in bytes.
 * @return Zeroed array allocation, or NULL when overflow or allocation fails.
 */
void *cwist_alloc_array(size_t count, size_t elem_size) {
    if (count == 0 || elem_size == 0) {
        return cwist_malloc(1);
    }
    if (elem_size > SIZE_MAX / count) {
        return NULL;
    }
    return cwist_malloc(count * elem_size);
}

/**
 * @brief Resize an existing allocation while preserving its previous contents.
 * @param ptr Existing allocation, or NULL to behave like malloc.
 * @param new_size Requested new size in bytes.
 * @return Resized allocation, or NULL when reallocation fails.
 */
void *cwist_realloc(void *ptr, size_t new_size) {
    size_t actual = new_size ? new_size : 1;
    if (!ptr) {
        return cwist_malloc(actual);
    }
    if (!g_owner_enabled) {
        void *res = realloc(ptr, actual);
        return res;
    }
    cwist_owner_realloc_args_t args = {
        .ptr = ptr,
        .size = actual,
        .result = NULL
    };
    if (!cwist_owner_call(CWIST_OWNER_REALLOC_FUNC, &args)) {
        return NULL;
    }
    return args.result;
}

/**
 * @brief Duplicate a NUL-terminated string into CWIST-managed storage.
 * @param src Source string to duplicate.
 * @return Duplicated string, or NULL when the source is NULL or allocation fails.
 */
char *cwist_strdup(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *dst = (char *)cwist_malloc(len + 1);
    if (!dst) return NULL;
    memcpy(dst, src, len + 1);
    return dst;
}

/**
 * @brief Count characters up to a bounded maximum without reading past the limit.
 * @param src Source string to scan.
 * @param max_len Maximum number of bytes to inspect.
 * @return Length up to the first NUL terminator or @p max_len.
 */
static size_t cwist_strnlen(const char *src, size_t max_len) {
    size_t len = 0;
    while (len < max_len && src[len]) {
        len++;
    }
    return len;
}

/**
 * @brief Duplicate at most @p n bytes of a string into CWIST-managed storage.
 * @param src Source string to duplicate.
 * @param n Maximum number of source bytes to copy.
 * @return Duplicated string, or NULL when the source is NULL or allocation fails.
 */
char *cwist_strndup(const char *src, size_t n) {
    if (!src) return NULL;
    size_t len = cwist_strnlen(src, n);
    char *dst = (char *)cwist_malloc(len + 1);
    if (!dst) return NULL;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

/**
 * @brief Free a CWIST-managed allocation, honoring guarded-owner mode when enabled.
 * @param ptr Allocation previously returned by CWIST memory helpers.
 */
void cwist_free(void *ptr) {
    if (!ptr) return;
    if (!g_owner_enabled) {
        free(ptr);
        return;
    }
    cwist_owner_free_args_t args = {.ptr = ptr};
    if (!cwist_owner_call(CWIST_OWNER_FREE_FUNC, &args)) {
        cwist_owner_abort(CWIST_OWNER_FREE_FUNC);
    }
}

/**
 * @brief Adapter that lets cJSON allocate through CWIST's memory layer.
 * @param size Requested allocation size in bytes.
 * @return Zeroed memory block, or NULL on failure.
 */
static void *cwist_cjson_malloc(size_t size) {
    return cwist_malloc(size);
}

/**
 * @brief Adapter that lets cJSON free memory through CWIST's memory layer.
 * @param ptr Allocation previously returned by CWIST's cJSON hook.
 */
static void cwist_cjson_free(void *ptr) {
    cwist_free(ptr);
}

/**
 * @brief Install cJSON hooks so JSON allocations share CWIST's memory strategy.
 */
__attribute__((constructor))
static void cwist_install_cjson_hooks(void) {
    cJSON_Hooks hooks = {
        .malloc_fn = cwist_cjson_malloc,
        .free_fn = cwist_cjson_free
    };
    cJSON_InitHooks(&hooks);
}

/**
 * @brief Tear down the shared owner context during process shutdown.
 */
static void cwist_destroy_owner(void) {
    if (!atomic_load_explicit(&g_owner_lock_ready, memory_order_acquire)) {
        return;
    }
    ttak_mutex_lock(&g_owner_lock);
    if (g_owner) {
        ttak_owner_destroy(g_owner);
        g_owner = NULL;
    }
    ttak_mutex_unlock(&g_owner_lock);
}

/**
 * @brief Destructor hook that releases the shared owner context on process exit.
 */
__attribute__((destructor))
static void cwist_owner_cleanup(void) {
    cwist_destroy_owner();
}
