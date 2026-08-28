/**
 * @file compress.c
 * @brief Compression middleware and built-in zlib/brotli/zstd backends.
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/compress.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/net/http/http.h>
#include <zlib.h>
#include <brotli/encode.h>
#include <zstd.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

#define CWIST_MAX_BACKENDS 8

static const cwist_compress_backend *g_backends[CWIST_MAX_BACKENDS];
static int g_backend_count = 0;
static size_t g_compress_min_size = 0;

void cwist_compress_register_backend(const cwist_compress_backend *backend) {
    if (!backend || g_backend_count >= CWIST_MAX_BACKENDS) return;
    g_backends[g_backend_count++] = backend;
}

void cwist_compress_unregister_all(void) {
    g_backend_count = 0;
}

/* --- zlib/gzip backend --- */

static int zlib_gzip_init(void **state) {
    z_stream *zs = (z_stream *)calloc(1, sizeof(z_stream));
    if (!zs) return -1;
    if (deflateInit2(zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(zs);
        return -1;
    }
    *state = zs;
    return 0;
}

static int zlib_deflate_init(void **state) {
    z_stream *zs = (z_stream *)calloc(1, sizeof(z_stream));
    if (!zs) return -1;
    if (deflateInit2(zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(zs);
        return -1;
    }
    *state = zs;
    return 0;
}

static int zlib_compress(void *state, const char *in, size_t in_len,
                         char *out, size_t *out_len, int flush) {
    z_stream *zs = (z_stream *)state;
    zs->next_in = (Bytef *)in;
    zs->avail_in = (uInt)in_len;
    zs->next_out = (Bytef *)out;
    zs->avail_out = (uInt)(*out_len);
    int rc = deflate(zs, flush ? Z_FINISH : Z_NO_FLUSH);
    /* All-or-nothing: the caller advances the input by the full chunk, so a
     * partially consumed chunk would silently drop the tail of the body.
     * Report an error instead; the middleware then serves the body
     * uncompressed. */
    if (flush) {
        if (rc != Z_STREAM_END) return -1;
    } else {
        if (rc != Z_OK || zs->avail_in != 0) return -1;
    }
    *out_len = (size_t)(*out_len - zs->avail_out);
    return 0;
}

static int zlib_finish(void *state, char *out, size_t *out_len) {
    z_stream *zs = (z_stream *)state;
    zs->next_in = NULL;
    zs->avail_in = 0;
    zs->next_out = (Bytef *)out;
    zs->avail_out = (uInt)(*out_len);
    int rc = deflate(zs, Z_FINISH);
    /* Z_BUF_ERROR here means the trailer did not fit; accepting it would
     * emit a truncated gzip stream. */
    if (rc != Z_STREAM_END) return -1;
    *out_len = (size_t)(*out_len - zs->avail_out);
    return 0;
}

static void zlib_cleanup(void *state) {
    z_stream *zs = (z_stream *)state;
    if (zs) {
        deflateEnd(zs);
        free(zs);
    }
}

static const cwist_compress_backend cwist_backend_gzip = {
    .encoding_name = "gzip",
    .init = zlib_gzip_init,
    .compress = zlib_compress,
    .finish = zlib_finish,
    .cleanup = zlib_cleanup,
};

static const cwist_compress_backend cwist_backend_deflate = {
    .encoding_name = "deflate",
    .init = zlib_deflate_init,
    .compress = zlib_compress,
    .finish = zlib_finish,
    .cleanup = zlib_cleanup,
};

const cwist_compress_backend *cwist_compress_backend_gzip(void) {
    return &cwist_backend_gzip;
}

const cwist_compress_backend *cwist_compress_backend_deflate(void) {
    return &cwist_backend_deflate;
}

/* --- Brotli backend --- */

typedef struct {
    char *accum;       /**< Input accumulated across non-flush calls. */
    size_t accum_len;
    size_t accum_cap;
    char *out;         /**< Compressed output buffer. */
    size_t out_len;
    size_t out_pos;
} cwist_brotli_state_t;

static int brotli_init(void **state) {
    cwist_brotli_state_t *bs = (cwist_brotli_state_t *)calloc(1, sizeof(*bs));
    if (!bs) return -1;
    *state = bs;
    return 0;
}

static int brotli_ensure_accum(cwist_brotli_state_t *bs, size_t need) {
    if (!bs) return -1;
    size_t required = bs->accum_len + need;
    if (required > bs->accum_cap) {
        size_t new_cap = bs->accum_cap ? bs->accum_cap * 2 : 4096;
        while (new_cap < required) new_cap *= 2;
        char *tmp = (char *)realloc(bs->accum, new_cap);
        if (!tmp) return -1;
        bs->accum = tmp;
        bs->accum_cap = new_cap;
    }
    return 0;
}

static int brotli_compress(void *state, const char *in, size_t in_len,
                           char *out, size_t *out_len, int flush) {
    cwist_brotli_state_t *bs = (cwist_brotli_state_t *)state;
    if (!bs || !out || !out_len) return -1;

    if (!flush) {
        if (in_len == 0) {
            *out_len = 0;
            return 0;
        }
        if (brotli_ensure_accum(bs, in_len) != 0) return -1;
        memcpy(bs->accum + bs->accum_len, in, in_len);
        bs->accum_len += in_len;
        *out_len = 0;
        return 0;
    }

    /* On flush, compress accumulated input plus the final chunk. */
    size_t total_in_len = bs->accum_len + in_len;
    const uint8_t *total_in = NULL;
    uint8_t *merged = NULL;
    if (bs->accum_len > 0) {
        merged = (uint8_t *)malloc(total_in_len);
        if (!merged) return -1;
        if (bs->accum_len > 0) memcpy(merged, bs->accum, bs->accum_len);
        if (in_len > 0) memcpy(merged + bs->accum_len, in, in_len);
        total_in = merged;
    } else {
        total_in = (const uint8_t *)in;
    }

    size_t encoded_size = BrotliEncoderMaxCompressedSize(total_in_len);
    if (encoded_size == 0) {
        free(merged);
        return -1;
    }

    uint8_t *encoded = (uint8_t *)malloc(encoded_size);
    if (!encoded) {
        free(merged);
        return -1;
    }

    if (!BrotliEncoderCompress(BROTLI_DEFAULT_QUALITY, BROTLI_DEFAULT_WINDOW,
                               BROTLI_DEFAULT_MODE, total_in_len, total_in,
                               &encoded_size, encoded)) {
        free(encoded);
        free(merged);
        return -1;
    }

    free(merged);
    bs->accum_len = 0;

    /* Replace any previous output buffer with the new one. */
    free(bs->out);
    bs->out = (char *)encoded;
    bs->out_len = encoded_size;
    bs->out_pos = 0;

    size_t to_copy = encoded_size;
    if (to_copy > *out_len) to_copy = *out_len;
    if (to_copy > 0 && out) memcpy(out, encoded, to_copy);
    bs->out_pos = to_copy;
    *out_len = to_copy;
    return 0;
}

static int brotli_finish(void *state, char *out, size_t *out_len) {
    cwist_brotli_state_t *bs = (cwist_brotli_state_t *)state;
    if (!bs || !out || !out_len) return -1;
    size_t remaining = bs->out_len - bs->out_pos;
    size_t to_copy = remaining;
    if (to_copy > *out_len) to_copy = *out_len;
    if (to_copy > 0) memcpy(out, bs->out + bs->out_pos, to_copy);
    bs->out_pos += to_copy;
    *out_len = to_copy;
    return 0;
}

static void brotli_cleanup(void *state) {
    cwist_brotli_state_t *bs = (cwist_brotli_state_t *)state;
    if (!bs) return;
    free(bs->accum);
    free(bs->out);
    free(bs);
}

static const cwist_compress_backend cwist_backend_brotli = {
    .encoding_name = "br",
    .init = brotli_init,
    .compress = brotli_compress,
    .finish = brotli_finish,
    .cleanup = brotli_cleanup,
};

const cwist_compress_backend *cwist_compress_backend_brotli(void) {
    return &cwist_backend_brotli;
}

/* --- Zstandard (zstd) backend --- */

typedef struct {
    ZSTD_CStream *cstream;
    char         *accum;
    size_t        accum_len;
    size_t        accum_cap;
    char         *out;
    size_t        out_len;
    size_t        out_pos;
} cwist_zstd_state_t;

static int zstd_init(void **state) {
    cwist_zstd_state_t *zs = (cwist_zstd_state_t *)calloc(1, sizeof(*zs));
    if (!zs) return -1;
    zs->cstream = ZSTD_createCStream();
    if (!zs->cstream) { free(zs); return -1; }
    size_t rc = ZSTD_initCStream(zs->cstream, ZSTD_CLEVEL_DEFAULT);
    if (ZSTD_isError(rc)) { ZSTD_freeCStream(zs->cstream); free(zs); return -1; }
    *state = zs;
    return 0;
}

static int zstd_ensure_accum(cwist_zstd_state_t *zs, size_t need) {
    size_t required = zs->accum_len + need;
    if (required > zs->accum_cap) {
        size_t new_cap = zs->accum_cap ? zs->accum_cap * 2 : 4096;
        while (new_cap < required) new_cap *= 2;
        char *tmp = (char *)realloc(zs->accum, new_cap);
        if (!tmp) return -1;
        zs->accum = tmp;
        zs->accum_cap = new_cap;
    }
    return 0;
}

static int zstd_compress(void *state, const char *in, size_t in_len,
                         char *out, size_t *out_len, int flush) {
    cwist_zstd_state_t *zs = (cwist_zstd_state_t *)state;
    if (!zs || !out || !out_len) return -1;

    if (!flush) {
        if (in_len == 0) { *out_len = 0; return 0; }
        if (zstd_ensure_accum(zs, in_len) != 0) return -1;
        memcpy(zs->accum + zs->accum_len, in, in_len);
        zs->accum_len += in_len;
        *out_len = 0;
        return 0;
    }

    size_t total_in_len = zs->accum_len + in_len;
    char *total_in = (char *)malloc(total_in_len);
    if (!total_in) return -1;
    if (zs->accum_len > 0) memcpy(total_in, zs->accum, zs->accum_len);
    if (in_len > 0) memcpy(total_in + zs->accum_len, in, in_len);

    size_t out_cap = ZSTD_compressBound(total_in_len);
    char *out_buf = (char *)malloc(out_cap);
    if (!out_buf) { free(total_in); return -1; }

    ZSTD_inBuffer  ib = { total_in, total_in_len, 0 };
    ZSTD_outBuffer ob = { out_buf, out_cap, 0 };

    size_t rc = ZSTD_compressStream2(zs->cstream, &ob, &ib, ZSTD_e_end);
    free(total_in);
    if (ZSTD_isError(rc)) { free(out_buf); return -1; }

    zs->accum_len = 0;
    free(zs->out);
    zs->out = out_buf;
    zs->out_len = ob.pos;
    zs->out_pos = 0;

    size_t to_copy = (ob.pos < *out_len) ? ob.pos : *out_len;
    memcpy(out, out_buf, to_copy);
    zs->out_pos = to_copy;
    *out_len = to_copy;
    return 0;
}

static int zstd_finish(void *state, char *out, size_t *out_len) {
    cwist_zstd_state_t *zs = (cwist_zstd_state_t *)state;
    if (!zs || !out || !out_len) return -1;
    size_t remaining = zs->out_len - zs->out_pos;
    size_t to_copy = (remaining < *out_len) ? remaining : *out_len;
    if (to_copy > 0) memcpy(out, zs->out + zs->out_pos, to_copy);
    zs->out_pos += to_copy;
    *out_len = to_copy;
    return 0;
}

static void zstd_cleanup(void *state) {
    cwist_zstd_state_t *zs = (cwist_zstd_state_t *)state;
    if (!zs) return;
    if (zs->cstream) ZSTD_freeCStream(zs->cstream);
    free(zs->accum);
    free(zs->out);
    free(zs);
}

static const cwist_compress_backend cwist_backend_zstd = {
    .encoding_name = "zstd",
    .init     = zstd_init,
    .compress = zstd_compress,
    .finish   = zstd_finish,
    .cleanup  = zstd_cleanup,
};

const cwist_compress_backend *cwist_compress_backend_zstd(void) {
    return &cwist_backend_zstd;
}
/* --- Middleware helpers --- */

static int str_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen == 0) return 1;
    if (hlen < nlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; ++i) {
        size_t j = 0;
        for (; j < nlen; ++j) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
            if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
            if (a != b) break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

static const cwist_compress_backend *select_backend(const char *accept_encoding) {
    if (!accept_encoding) return NULL;
    /* Prefer gzip, then deflate */
    for (int i = 0; i < g_backend_count; ++i) {
        if (str_contains_ci(accept_encoding, g_backends[i]->encoding_name)) {
            return g_backends[i];
        }
    }
    return NULL;
}

static void remove_header(cwist_http_header_node **head, const char *key) {
    cwist_http_header_node **curr = head;
    while (*curr) {
        if ((*curr)->key && (*curr)->key->data && strcasecmp((*curr)->key->data, key) == 0) {
            cwist_http_header_node *to_free = *curr;
            *curr = (*curr)->next;
            cwist_sstring_destroy(to_free->key);
            cwist_sstring_destroy(to_free->value);
            if (!to_free->arena_owned) {
                cwist_free(to_free);
            }
            continue;
        }
        curr = &(*curr)->next;
    }
}

static void cwist_mw_compress_handler(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    const char *accept = cwist_http_header_get(req->headers, "Accept-Encoding");
    const cwist_compress_backend *backend = select_backend(accept);

    next(req, res);

    if (!backend) return;
    if (!res->body || res->body->size == 0) return;
    if (res->is_ptr_body || res->use_file_stream) return;
    if (res->status_code < 200 || res->status_code >= 300) return;
    if (res->body->size < g_compress_min_size) return;

    void *state = NULL;
    if (backend->init(&state) != 0) return;

    size_t out_cap = res->body->size + 256;
    char *out_buf = (char *)cwist_malloc(out_cap);
    if (!out_buf) {
        backend->cleanup(state);
        return;
    }

    size_t total_out = 0;
    int ok = 1;

    /* Stream the entire body through the compressor */
    size_t in_offset = 0;
    while (in_offset < res->body->size && ok) {
        size_t chunk_in = res->body->size - in_offset;
        size_t chunk_out = out_cap - total_out;
        if (chunk_out == 0) {
            /* Grow output buffer */
            size_t new_cap = out_cap * 2;
            char *new_buf = (char *)cwist_realloc(out_buf, new_cap);
            if (!new_buf) { ok = 0; break; }
            out_buf = new_buf;
            out_cap = new_cap;
            chunk_out = out_cap - total_out;
        }
        int flush = (in_offset + chunk_in >= res->body->size) ? 1 : 0;
        size_t written = chunk_out;
        if (backend->compress(state, res->body->data + in_offset, chunk_in,
                              out_buf + total_out, &written, flush) != 0) {
            ok = 0;
            break;
        }
        total_out += written;
        in_offset += chunk_in;
    }

    if (ok) {
        /* Drain trailing bytes, growing the buffer until the backend is
         * exhausted (brotli/zstd buffer the whole stream internally, so a
         * single fixed-size tail can truncate the output). */
        for (;;) {
            size_t tail = out_cap - total_out;
            if (tail == 0) {
                size_t new_cap = out_cap * 2;
                char *new_buf = (char *)cwist_realloc(out_buf, new_cap);
                if (!new_buf) { ok = 0; break; }
                out_buf = new_buf;
                out_cap = new_cap;
                continue;
            }
            size_t wrote = tail;
            if (backend->finish(state, out_buf + total_out, &wrote) != 0) {
                ok = 0;
                break;
            }
            total_out += wrote;
            if (wrote < tail) break; /* drained */
        }
    }

    backend->cleanup(state);

    if (ok && total_out > 0 && total_out < res->body->size) {
        cwist_sstring_assign_len(res->body, out_buf, total_out);
        cwist_http_header_add(&res->headers, "Content-Encoding", backend->encoding_name);
        remove_header(&res->headers, "Content-Length");
    }
    cwist_free(out_buf);
}

cwist_middleware_func cwist_mw_compress(size_t min_body_size) {
    g_compress_min_size = min_body_size;
    return cwist_mw_compress_handler;
}
