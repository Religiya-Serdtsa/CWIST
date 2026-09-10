/**
 * @file typedarray.h
 * @brief Emscripten-only zero-copy helpers: expose C arrays and in-memory
 * dispatch responses to JS as TypedArray views over the WASM heap, instead
 * of round-tripping through snprintf/JSON.
 *
 * This header is a deliberate no-op on native toolchains: every definition
 * is guarded by __EMSCRIPTEN__, so including it from shared code is safe.
 *
 * JS side, once per module:
 * @code
 *   cwist_wasm_install_views();            // C, from main() — installs:
 *   Module.cwistView.u8(ptr, len)          //   -> Uint8Array view
 *   Module.cwistView.i32(ptr, len)         //   -> Int32Array view
 *   Module.cwistView.f64(ptr, len)         //   -> Float64Array view
 * @endcode
 */

#ifndef __CWIST_WASM_TYPEDARRAY_H__
#define __CWIST_WASM_TYPEDARRAY_H__

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <stddef.h>
#include <cwist/sys/app/app.h>
#include <cwist/core/mem/alloc.h>

/** Export marker for functions JS should be able to call via ccall. */
#define CWIST_WASM_EXPORT EMSCRIPTEN_KEEPALIVE

/**
 * Expose a static C array to JS as a (pointer, length) getter pair.
 * The array must outlive the views (static storage or heap you keep alive).
 * JS side for `CWIST_WASM_EXPOSE_I32(samples, g_samples, 4)`:
 *   Module.cwistView.i32(_samples_ptr(), _samples_len());
 * or, with -sEXPORTED_RUNTIME_METHODS=HEAP32, directly:
 *   new Int32Array(Module.HEAP32.buffer, _samples_ptr(), _samples_len());
 * The typed aliases only document the intended JS view type; the C side is
 * always an opaque (ptr, len) pair.
 */
#define CWIST_WASM_EXPOSE_ARRAY(name, arr, count)                          \
    CWIST_WASM_EXPORT const void *name##_ptr(void) {                       \
        return (const void *)(arr);                                        \
    }                                                                      \
    CWIST_WASM_EXPORT size_t name##_len(void) {                            \
        return (size_t)(count);                                            \
    }
#define CWIST_WASM_EXPOSE_I32(name, arr, count) CWIST_WASM_EXPOSE_ARRAY(name, arr, count)
#define CWIST_WASM_EXPOSE_F64(name, arr, count) CWIST_WASM_EXPOSE_ARRAY(name, arr, count)
#define CWIST_WASM_EXPOSE_U8(name, arr, count)  CWIST_WASM_EXPOSE_ARRAY(name, arr, count)

/**
 * Define cwist_wasm_install_views(), which registers Module.cwistView on the
 * JS side.  Expand once in a single translation unit (EM_JS emits a JS
 * function definition), then call it early from main().
 */
#define CWIST_WASM_INSTALL_VIEWS()                                          \
    EM_JS(void, cwist_wasm_install_views, (void), {                         \
        /* HEAP* bindings are in scope inside EM_JS-generated code. */      \
        if (typeof Module !== "undefined" && !Module.cwistView) {           \
            Module.cwistView = {                                            \
                u8: function (p, n) {                                       \
                    return new Uint8Array(HEAPU8.buffer, p, n);             \
                },                                                          \
                i32: function (p, n) {                                      \
                    return new Int32Array(HEAP32.buffer, p, n);             \
                },                                                          \
                f64: function (p, n) {                                      \
                    return new Float64Array(HEAPF64.buffer, p, n);          \
                },                                                          \
            };                                                              \
        }                                                                   \
    })

/**
 * Run the in-memory dispatcher and hand back the serialized response as
 * (pointer, length) living in the WASM heap — JS wraps it directly:
 *   Module.cwistView.u8(_my_dispatch(req_ptr, req_len, out_len_ptr), len)
 * Free the returned buffer with cwist_wasm_free() (from C) once JS is done.
 * Returns NULL on dispatch failure.
 */
static inline const char *cwist_wasm_dispatch_memory(cwist_app *app,
                                                     const char *req_buf,
                                                     size_t req_len,
                                                     size_t *res_len) {
    if (!res_len) return NULL;
    *res_len = 0;
    char *res_buf = NULL;
    if (cwist_app_dispatch_memory(app, req_buf, req_len, &res_buf, res_len) != 0)
        return NULL;
    return res_buf;
}

/** Free a buffer returned by cwist_wasm_dispatch_memory(). */
static inline void cwist_wasm_free(void *ptr) {
    cwist_free(ptr);
}

#else /* !__EMSCRIPTEN__ */

/* Native builds: this header intentionally provides nothing.  The helpers
 * only make sense against Emscripten's HEAP views. */

#endif /* __EMSCRIPTEN__ */

#endif /* __CWIST_WASM_TYPEDARRAY_H__ */
