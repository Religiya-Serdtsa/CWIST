/**
 * @file compress.h
 * @brief Compression middleware with swappable backend interface.
 */

#ifndef __CWIST_COMPRESS_H__
#define __CWIST_COMPRESS_H__

#include <cwist/sys/app/app.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Compression backend vtable.
 *
 * Implementations provide init/compress/finish/cleanup lifecycle hooks.
 * The middleware uses this interface so backends can be swapped or extended
 * (e.g., Brotli, zstd) without changing middleware logic.
 */
typedef struct cwist_compress_backend {
    const char *encoding_name; /**< e.g. "gzip", "deflate" */

    /** @brief Initialize compressor state. */
    int (*init)(void **state);

    /**
     * @brief Compress one chunk.
     * @param state   Opaque state from init().
     * @param in      Input bytes.
     * @param in_len  Input length.
     * @param out     Output buffer.
     * @param out_len In: capacity. Out: bytes written.
     * @param flush   Non-zero on final chunk.
     * @return 0 on success, -1 on error.
     */
    int (*compress)(void *state, const char *in, size_t in_len,
                    char *out, size_t *out_len, int flush);

    /**
     * @brief Finish compression and write any trailing bytes.
     * @param state   Opaque state from init().
     * @param out     Output buffer.
     * @param out_len In: capacity. Out: bytes written.
     * @return 0 on success, -1 on error.
     */
    int (*finish)(void *state, char *out, size_t *out_len);

    /** @brief Clean up compressor state. */
    void (*cleanup)(void *state);
} cwist_compress_backend;

/**
 * @brief Register a compression backend.
 * @param backend Pointer to a statically-allocated backend descriptor.
 */
void cwist_compress_register_backend(const cwist_compress_backend *backend);

/**
 * @brief Unregister all compression backends.
 */
void cwist_compress_unregister_all(void);

/**
 * @brief Returns the built-in zlib/gzip backend.
 */
const cwist_compress_backend *cwist_compress_backend_gzip(void);

/**
 * @brief Returns the built-in zlib/deflate backend.
 */
const cwist_compress_backend *cwist_compress_backend_deflate(void);

/**
 * @brief Returns the built-in Brotli backend.
 */
const cwist_compress_backend *cwist_compress_backend_brotli(void);

/**
 * @brief Returns the built-in Zstandard (zstd) backend.
 */
const cwist_compress_backend *cwist_compress_backend_zstd(void);

/**
 * @brief Compression middleware factory.
 *
 * Chooses a registered backend based on the request's Accept-Encoding header,
 * runs the handler chain, then compresses the response body.
 *
 * @param min_body_size Minimum uncompressed body size (in bytes) to trigger compression.
 *                      Use 0 to compress all bodies.
 * @return Middleware function pointer.
 */
cwist_middleware_func cwist_mw_compress(size_t min_body_size);

#ifdef __cplusplus
}
#endif

#endif
