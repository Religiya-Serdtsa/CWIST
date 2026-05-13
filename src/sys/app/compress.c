/**
 * @file compress.c
 * @brief Compression middleware and built-in zlib backends.
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/compress.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/net/http/http.h>
#include <zlib.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define CWIST_MAX_BACKENDS 4

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
    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) return -1;
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
    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) return -1;
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
            cwist_free(to_free);
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
    char *out_buf = (char *)malloc(out_cap);
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
            char *new_buf = (char *)realloc(out_buf, new_cap);
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
        size_t tail = out_cap - total_out;
        if (tail == 0) {
            size_t new_cap = out_cap + 256;
            char *new_buf = (char *)realloc(out_buf, new_cap);
            if (new_buf) {
                out_buf = new_buf;
                out_cap = new_cap;
                tail = out_cap - total_out;
            }
        }
        if (tail > 0) {
            if (backend->finish(state, out_buf + total_out, &tail) != 0) {
                ok = 0;
            } else {
                total_out += tail;
            }
        }
    }

    backend->cleanup(state);

    if (ok && total_out > 0 && total_out < res->body->size) {
        cwist_sstring_assign_len(res->body, out_buf, total_out);
        cwist_http_header_add(&res->headers, "Content-Encoding", backend->encoding_name);
        remove_header(&res->headers, "Content-Length");
    }

    free(out_buf);
}

cwist_middleware_func cwist_mw_compress(size_t min_body_size) {
    g_compress_min_size = min_body_size;
    return cwist_mw_compress_handler;
}
