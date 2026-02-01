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

#define CWIST_OWNER_RESOURCE "cwist_mem_resource"
#define CWIST_OWNER_ALLOC_FUNC "cwist_mem_alloc"
#define CWIST_OWNER_FREE_FUNC "cwist_mem_free"
#define CWIST_OWNER_REALLOC_FUNC "cwist_mem_realloc"

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
static atomic_bool g_owner_lock_ready = ATOMIC_VAR_INIT(false);
static ttak_owner_t *g_owner = NULL;
static cwist_owner_policy_t g_owner_policy = {
    .flags = TTAK_MEM_DEFAULT | TTAK_MEM_STRICT_CHECK
};
static bool g_owner_enabled = false;

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

static void cwist_owner_free(void *ctx, void *args) {
    (void)ctx;
    cwist_owner_free_args_t *req = (cwist_owner_free_args_t *)args;
    if (!req || !req->ptr) return;
    ttak_mem_free(req->ptr);
    req->ptr = NULL;
}

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

static void cwist_owner_abort(const char *func_name) {
    fprintf(stderr, "[CWIST] Unable to honor %s; aborting to prevent unsafe memory access\n", func_name);
    abort();
}

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

void *cwist_alloc(size_t size) {
    return cwist_malloc(size);
}

void *cwist_alloc_array(size_t count, size_t elem_size) {
    if (count == 0 || elem_size == 0) {
        return cwist_malloc(1);
    }
    if (elem_size > SIZE_MAX / count) {
        return NULL;
    }
    return cwist_malloc(count * elem_size);
}

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

char *cwist_strdup(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *dst = (char *)cwist_malloc(len + 1);
    if (!dst) return NULL;
    memcpy(dst, src, len + 1);
    return dst;
}

static size_t cwist_strnlen(const char *src, size_t max_len) {
    size_t len = 0;
    while (len < max_len && src[len]) {
        len++;
    }
    return len;
}

char *cwist_strndup(const char *src, size_t n) {
    if (!src) return NULL;
    size_t len = cwist_strnlen(src, n);
    char *dst = (char *)cwist_malloc(len + 1);
    if (!dst) return NULL;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

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

static void *cwist_cjson_malloc(size_t size) {
    return cwist_malloc(size);
}

static void cwist_cjson_free(void *ptr) {
    cwist_free(ptr);
}

__attribute__((constructor))
static void cwist_install_cjson_hooks(void) {
    cJSON_Hooks hooks = {
        .malloc_fn = cwist_cjson_malloc,
        .free_fn = cwist_cjson_free
    };
    cJSON_InitHooks(&hooks);
}

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

__attribute__((destructor))
static void cwist_owner_cleanup(void) {
    cwist_destroy_owner();
}
