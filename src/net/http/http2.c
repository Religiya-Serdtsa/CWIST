/**
 * @file http2.c
 * @brief Implementation of HTTP/2 protocol handler for CWIST.
 * @author Lee Yunjin
 * @date 2026-04-27
 */

#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <cwist/net/http/http2.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/sys/err/cwist_err.h>
#include <cwist/sys/app/shutdown.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

/* --- Internal Macros --- */
#define CWIST_HTTP2_FRAME_HEADER_SIZE 9
#define CWIST_HTTP2_CONNECTION_PREFACE "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define CWIST_HTTP2_CONNECTION_PREFACE_LEN 24
#define CWIST_HTTP2_MAX_FRAME_SIZE 16384
#define CWIST_HTTP2_MAX_CONCURRENT_STREAMS 32

#define CWIST_HTTP2_FLAG_END_STREAM 0x01
#define CWIST_HTTP2_FLAG_END_HEADERS 0x04
#define CWIST_HTTP2_FLAG_PADDED 0x08
#define CWIST_HTTP2_FLAG_PRIORITY 0x20
#define CWIST_HTTP2_FLAG_ACK 0x01
#define CWIST_HTTP2_FRAME_DATA 0x00
#define CWIST_HTTP2_FRAME_HEADERS 0x01
#define CWIST_HTTP2_FRAME_SETTINGS 0x04

/**
 * @brief Internal state for an HTTP/2 connection.
 *
 * Maintains the SSL context, connection preface status,
 * and stream management information.
 */
typedef struct {
    SSL *ssl;                 /**< Pointer to the underlying BoringSSL SSL object */
    bool preface_received;    /**< Flag indicating if the HTTP/2 connection preface was received */
    uint32_t last_stream_id;  /**< The highest stream ID seen on this connection */
} cwist_http2_conn_internal;

/* --- Private Function Prototypes --- */

typedef struct {
    const char *name;
    const char *value;
} cwist_http2_static_header;

static const cwist_http2_static_header cwist_http2_static_table[] = {
    { NULL, NULL },
    { ":authority", "" },
    { ":method", "GET" },
    { ":method", "POST" },
    { ":path", "/" },
    { ":path", "/index.html" },
    { ":scheme", "http" },
    { ":scheme", "https" },
    { ":status", "200" },
    { ":status", "204" },
    { ":status", "206" },
    { ":status", "304" },
    { ":status", "400" },
    { ":status", "404" },
    { ":status", "500" },
    { "accept-charset", "" },
    { "accept-encoding", "gzip, deflate" },
    { "accept-language", "" },
    { "accept-ranges", "" },
    { "accept", "" },
    { "access-control-allow-origin", "" },
    { "age", "" },
    { "allow", "" },
    { "authorization", "" },
    { "cache-control", "" },
    { "content-disposition", "" },
    { "content-encoding", "" },
    { "content-language", "" },
    { "content-length", "" },
    { "content-location", "" },
    { "content-range", "" },
    { "content-type", "" },
    { "cookie", "" },
    { "date", "" },
    { "etag", "" },
    { "expect", "" },
    { "expires", "" },
    { "from", "" },
    { "host", "" },
    { "if-match", "" },
    { "if-modified-since", "" },
    { "if-none-match", "" },
    { "if-range", "" },
    { "if-unmodified-since", "" },
    { "last-modified", "" },
    { "link", "" },
    { "location", "" },
    { "max-forwards", "" },
    { "proxy-authenticate", "" },
    { "proxy-authorization", "" },
    { "range", "" },
    { "referer", "" },
    { "refresh", "" },
    { "retry-after", "" },
    { "server", "" },
    { "set-cookie", "" },
    { "strict-transport-security", "" },
    { "transfer-encoding", "" },
    { "user-agent", "" },
    { "vary", "" },
    { "via", "" },
    { "www-authenticate", "" },
};

/**
 * @brief Read bytes from the underlying connection.
 */
static int h2_read(cwist_https_connection *conn, void *buf, int len) {
    if (conn->ssl) return SSL_read(conn->ssl, buf, len);
    return read(conn->fd, buf, len);
}

/**
 * @brief Write bytes to the underlying connection.
 */
static int h2_write(cwist_https_connection *conn, const void *buf, int len) {
    if (conn->ssl) return SSL_write(conn->ssl, buf, len);
    return write(conn->fd, buf, len);
}

/**
 * @brief Write the entire buffer, looping until completion.
 */
static int h2_write_all(cwist_https_connection *conn, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    while (len > 0) {
        int n = h2_write(conn, p, (int)len);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/**
 * @brief Encode an HTTP/2 frame header.
 */
static void h2_make_frame_header(unsigned char out[CWIST_HTTP2_FRAME_HEADER_SIZE],
                                 uint32_t len,
                                 uint8_t type,
                                 uint8_t flags,
                                 uint32_t stream_id) {
    out[0] = (unsigned char)((len >> 16) & 0xff);
    out[1] = (unsigned char)((len >> 8) & 0xff);
    out[2] = (unsigned char)(len & 0xff);
    out[3] = type;
    out[4] = flags;
    out[5] = (unsigned char)((stream_id >> 24) & 0x7f);
    out[6] = (unsigned char)((stream_id >> 16) & 0xff);
    out[7] = (unsigned char)((stream_id >> 8) & 0xff);
    out[8] = (unsigned char)(stream_id & 0xff);
}

/**
 * @brief Send a complete HTTP/2 frame.
 */
static int h2_write_frame(cwist_https_connection *conn,
                          uint8_t type,
                          uint8_t flags,
                          uint32_t stream_id,
                          const unsigned char *payload,
                          uint32_t len) {
    unsigned char frame[CWIST_HTTP2_FRAME_HEADER_SIZE];
    h2_make_frame_header(frame, len, type, flags, stream_id);
    if (h2_write_all(conn, frame, sizeof(frame)) != 0) return -1;
    if (len > 0 && payload) return h2_write_all(conn, payload, len);
    return 0;
}

int h2_decode_integer(const unsigned char *buf,
                             size_t len,
                             size_t *pos,
                             uint8_t prefix_bits,
                             uint32_t *value) {
    if (*pos >= len || prefix_bits == 0 || prefix_bits > 8) return -1;
    uint8_t mask = (uint8_t)((1u << prefix_bits) - 1u);
    uint32_t n = buf[*pos] & mask;
    (*pos)++;
    if (n < mask) {
        *value = n;
        return 0;
    }

    uint32_t m = 0;
    while (*pos < len) {
        uint8_t b = buf[(*pos)++];
        if (m > 28) return -1;
        n += (uint32_t)(b & 0x7f) << m;
        if ((b & 0x80) == 0) {
            *value = n;
            return 0;
        }
        m += 7;
    }
    return -1;
}

/* --- Huffman Decoder (RFC 7541 Appendix B) --- */

typedef struct h2_huffman_node {
    struct h2_huffman_node *child[2];
    int symbol;
    bool is_terminal;
} h2_huffman_node;

static h2_huffman_node h2_huffman_pool[4096];
static size_t h2_huffman_pool_used = 0;
static h2_huffman_node *h2_huffman_root = NULL;

static h2_huffman_node *h2_huffman_alloc_node(void) {
    if (h2_huffman_pool_used >= sizeof(h2_huffman_pool)/sizeof(h2_huffman_pool[0]))
        return NULL;
    h2_huffman_node *node = &h2_huffman_pool[h2_huffman_pool_used++];
    node->child[0] = node->child[1] = NULL;
    node->symbol = -1;
    node->is_terminal = false;
    return node;
}

static void h2_huffman_init(void) {
    if (h2_huffman_root) return;

static const struct {
    uint32_t code;
    uint8_t bits;
} table[257] = {
    {0x00001ff8, 13},
    {0x007fffd8, 23},
    {0x0fffffe2, 28},
    {0x0fffffe3, 28},
    {0x0fffffe4, 28},
    {0x0fffffe5, 28},
    {0x0fffffe6, 28},
    {0x0fffffe7, 28},
    {0x0fffffe8, 28},
    {0x00ffffea, 24},
    {0x3ffffffc, 30},
    {0x0fffffe9, 28},
    {0x0fffffea, 28},
    {0x3ffffffd, 30},
    {0x0fffffeb, 28},
    {0x0fffffec, 28},
    {0x0fffffed, 28},
    {0x0fffffee, 28},
    {0x0fffffef, 28},
    {0x0ffffff0, 28},
    {0x0ffffff1, 28},
    {0x0ffffff2, 28},
    {0x3ffffffe, 30},
    {0x0ffffff3, 28},
    {0x0ffffff4, 28},
    {0x0ffffff5, 28},
    {0x0ffffff6, 28},
    {0x0ffffff7, 28},
    {0x0ffffff8, 28},
    {0x0ffffff9, 28},
    {0x0ffffffa, 28},
    {0x0ffffffb, 28},
    {0x00000014,  6},
    {0x000003f8, 10},
    {0x000003f9, 10},
    {0x00000ffa, 12},
    {0x00001ff9, 13},
    {0x00000015,  6},
    {0x000000f8,  8},
    {0x000007fa, 11},
    {0x000003fa, 10},
    {0x000003fb, 10},
    {0x000000f9,  8},
    {0x000007fb, 11},
    {0x000000fa,  8},
    {0x00000016,  6},
    {0x00000017,  6},
    {0x00000018,  6},
    {0x00000000,  5},
    {0x00000001,  5},
    {0x00000002,  5},
    {0x00000019,  6},
    {0x0000001a,  6},
    {0x0000001b,  6},
    {0x0000001c,  6},
    {0x0000001d,  6},
    {0x0000001e,  6},
    {0x0000001f,  6},
    {0x0000005c,  7},
    {0x000000fb,  8},
    {0x00007ffc, 15},
    {0x00000020,  6},
    {0x00000ffb, 12},
    {0x000003fc, 10},
    {0x00001ffa, 13},
    {0x00000021,  6},
    {0x0000005d,  7},
    {0x0000005e,  7},
    {0x0000005f,  7},
    {0x00000060,  7},
    {0x00000061,  7},
    {0x00000062,  7},
    {0x00000063,  7},
    {0x00000064,  7},
    {0x00000065,  7},
    {0x00000066,  7},
    {0x00000067,  7},
    {0x00000068,  7},
    {0x00000069,  7},
    {0x0000006a,  7},
    {0x0000006b,  7},
    {0x0000006c,  7},
    {0x0000006d,  7},
    {0x0000006e,  7},
    {0x0000006f,  7},
    {0x00000070,  7},
    {0x00000071,  7},
    {0x00000072,  7},
    {0x000000fc,  8},
    {0x00000073,  7},
    {0x000000fd,  8},
    {0x00001ffb, 13},
    {0x0007fff0, 19},
    {0x00001ffc, 13},
    {0x00003ffc, 14},
    {0x00000022,  6},
    {0x00007ffd, 15},
    {0x00000003,  5},
    {0x00000023,  6},
    {0x00000004,  5},
    {0x00000024,  6},
    {0x00000005,  5},
    {0x00000025,  6},
    {0x00000026,  6},
    {0x00000027,  6},
    {0x00000006,  5},
    {0x00000074,  7},
    {0x00000075,  7},
    {0x00000028,  6},
    {0x00000029,  6},
    {0x0000002a,  6},
    {0x00000007,  5},
    {0x0000002b,  6},
    {0x00000076,  7},
    {0x0000002c,  6},
    {0x00000008,  5},
    {0x00000009,  5},
    {0x0000002d,  6},
    {0x00000077,  7},
    {0x00000078,  7},
    {0x00000079,  7},
    {0x0000007a,  7},
    {0x0000007b,  7},
    {0x00007ffe, 15},
    {0x000007fc, 11},
    {0x00003ffd, 14},
    {0x00001ffd, 13},
    {0x0ffffffc, 28},
    {0x000fffe6, 20},
    {0x003fffd2, 22},
    {0x000fffe7, 20},
    {0x000fffe8, 20},
    {0x003fffd3, 22},
    {0x003fffd4, 22},
    {0x003fffd5, 22},
    {0x007fffd9, 23},
    {0x003fffd6, 22},
    {0x007fffda, 23},
    {0x007fffdb, 23},
    {0x007fffdc, 23},
    {0x007fffdd, 23},
    {0x007fffde, 23},
    {0x00ffffeb, 24},
    {0x007fffdf, 23},
    {0x00ffffec, 24},
    {0x00ffffed, 24},
    {0x003fffd7, 22},
    {0x007fffe0, 23},
    {0x00ffffee, 24},
    {0x007fffe1, 23},
    {0x007fffe2, 23},
    {0x007fffe3, 23},
    {0x007fffe4, 23},
    {0x001fffdc, 21},
    {0x003fffd8, 22},
    {0x007fffe5, 23},
    {0x003fffd9, 22},
    {0x007fffe6, 23},
    {0x007fffe7, 23},
    {0x00ffffef, 24},
    {0x003fffda, 22},
    {0x001fffdd, 21},
    {0x000fffe9, 20},
    {0x003fffdb, 22},
    {0x003fffdc, 22},
    {0x007fffe8, 23},
    {0x007fffe9, 23},
    {0x001fffde, 21},
    {0x007fffea, 23},
    {0x003fffdd, 22},
    {0x003fffde, 22},
    {0x00fffff0, 24},
    {0x001fffdf, 21},
    {0x003fffdf, 22},
    {0x007fffeb, 23},
    {0x007fffec, 23},
    {0x001fffe0, 21},
    {0x001fffe1, 21},
    {0x003fffe0, 22},
    {0x001fffe2, 21},
    {0x007fffed, 23},
    {0x003fffe1, 22},
    {0x007fffee, 23},
    {0x007fffef, 23},
    {0x000fffea, 20},
    {0x003fffe2, 22},
    {0x003fffe3, 22},
    {0x003fffe4, 22},
    {0x007ffff0, 23},
    {0x003fffe5, 22},
    {0x003fffe6, 22},
    {0x007ffff1, 23},
    {0x03ffffe0, 26},
    {0x03ffffe1, 26},
    {0x000fffeb, 20},
    {0x0007fff1, 19},
    {0x003fffe7, 22},
    {0x007ffff2, 23},
    {0x003fffe8, 22},
    {0x01ffffec, 25},
    {0x03ffffe2, 26},
    {0x03ffffe3, 26},
    {0x03ffffe4, 26},
    {0x07ffffde, 27},
    {0x07ffffdf, 27},
    {0x03ffffe5, 26},
    {0x00fffff1, 24},
    {0x01ffffed, 25},
    {0x0007fff2, 19},
    {0x001fffe3, 21},
    {0x03ffffe6, 26},
    {0x07ffffe0, 27},
    {0x07ffffe1, 27},
    {0x03ffffe7, 26},
    {0x07ffffe2, 27},
    {0x00fffff2, 24},
    {0x001fffe4, 21},
    {0x001fffe5, 21},
    {0x03ffffe8, 26},
    {0x03ffffe9, 26},
    {0x0ffffffd, 28},
    {0x07ffffe3, 27},
    {0x07ffffe4, 27},
    {0x07ffffe5, 27},
    {0x000fffec, 20},
    {0x00fffff3, 24},
    {0x000fffed, 20},
    {0x001fffe6, 21},
    {0x003fffe9, 22},
    {0x001fffe7, 21},
    {0x001fffe8, 21},
    {0x007ffff3, 23},
    {0x003fffea, 22},
    {0x003fffeb, 22},
    {0x01ffffee, 25},
    {0x01ffffef, 25},
    {0x00fffff4, 24},
    {0x00fffff5, 24},
    {0x03ffffea, 26},
    {0x007ffff4, 23},
    {0x03ffffeb, 26},
    {0x07ffffe6, 27},
    {0x03ffffec, 26},
    {0x03ffffed, 26},
    {0x07ffffe7, 27},
    {0x07ffffe8, 27},
    {0x07ffffe9, 27},
    {0x07ffffea, 27},
    {0x07ffffeb, 27},
    {0x0ffffffe, 28},
    {0x07ffffec, 27},
    {0x07ffffed, 27},
    {0x07ffffee, 27},
    {0x07ffffef, 27},
    {0x07fffff0, 27},
    {0x03ffffee, 26},
    {0x3fffffff, 30},
};

    h2_huffman_root = h2_huffman_alloc_node();
    for (int sym = 0; sym <= 256; ++sym) {
        uint32_t code = table[sym].code;
        uint8_t bits = table[sym].bits;
        h2_huffman_node *node = h2_huffman_root;
        for (int i = bits - 1; i >= 0; --i) {
            int bit = (int)((code >> i) & 1);
            if (!node->child[bit]) node->child[bit] = h2_huffman_alloc_node();
            node = node->child[bit];
        }
        node->symbol = sym;
        node->is_terminal = true;
    }
}

char *h2_huffman_decode(const unsigned char *src, size_t src_len, size_t *out_len) {
    h2_huffman_init();
    size_t cap = src_len * 2 + 1;
    if (cap < 16) cap = 16;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t out_pos = 0;

    h2_huffman_node *node = h2_huffman_root;
    for (size_t i = 0; i < src_len; ++i) {
        unsigned char byte = src[i];
        for (int b = 7; b >= 0; --b) {
            int bit = (byte >> b) & 1;
            node = node->child[bit];
            if (!node) { free(out); return NULL; }
            if (node->is_terminal) {
                if (node->symbol == 256) {
                    /* EOS reached before end of input - error per RFC 7541 */
                    free(out);
                    return NULL;
                }
                if (out_pos + 1 >= cap) {
                    cap *= 2;
                    char *tmp = (char *)realloc(out, cap);
                    if (!tmp) { free(out); return NULL; }
                    out = tmp;
                }
                out[out_pos++] = (char)node->symbol;
                node = h2_huffman_root;
            }
        }
    }

    /* If we ended mid-tree, the remaining bits must be a prefix of EOS (all 1s).
     * Check that the current node is on the path to EOS. */
    if (node != h2_huffman_root) {
        /* Walk the remaining bits as 1s to see if we reach EOS */
        h2_huffman_node *check = node;
        while (check && !check->is_terminal) {
            check = check->child[1];
        }
        if (!check || check->symbol != 256) {
            free(out);
            return NULL;
        }
    }

    out[out_pos] = '\0';
    if (out_len) *out_len = out_pos;
    return out;
}

char *h2_decode_string(const unsigned char *buf, size_t len, size_t *pos) {
    if (*pos >= len) return NULL;
    bool huffman = (buf[*pos] & 0x80) != 0;
    uint32_t str_len = 0;
    if (h2_decode_integer(buf, len, pos, 7, &str_len) != 0) return NULL;
    if (*pos + str_len > len) return NULL;

    if (huffman) {
        size_t decoded_len = 0;
        char *out = h2_huffman_decode(buf + *pos, str_len, &decoded_len);
        *pos += str_len;
        return out;
    }

    char *out = (char *)malloc((size_t)str_len + 1);
    if (!out) return NULL;
    memcpy(out, buf + *pos, str_len);
    out[str_len] = '\0';
    *pos += str_len;
    return out;
}

/**
 * @brief Look up a static table entry by index.
 */
static const cwist_http2_static_header *h2_static_header(uint32_t index) {
    size_t count = sizeof(cwist_http2_static_table) / sizeof(cwist_http2_static_table[0]);
    if (index == 0 || index >= count) return NULL;
    return &cwist_http2_static_table[index];
}

/**
 * @brief Parse the :path pseudo-header into path and query components.
 */
static void h2_parse_path(cwist_http_request *req, const char *path) {
    const char *q = strchr(path, '?');
    if (q) {
        cwist_sstring_assign_len(req->path, (char *)path, (size_t)(q - path));
        cwist_sstring_assign(req->query, (char *)(q + 1));
        if (req->query_params) {
            cwist_query_map_clear(req->query_params);
        } else {
            req->query_params = cwist_query_map_create();
        }
        if (req->query_params && req->query->size > 0) {
            cwist_query_map_parse(req->query_params, req->query->data);
        }
    } else {
        cwist_sstring_assign(req->path, (char *)path);
    }
}

/**
 * @brief Apply a decoded header to the request object.
 */
static void h2_apply_header(cwist_http_request *req, const char *name, const char *value) {
    if (!req || !name || !value) return;

    if (strcmp(name, ":method") == 0) {
        req->method = cwist_http_string_to_method(value);
    } else if (strcmp(name, ":path") == 0) {
        h2_parse_path(req, value);
    } else if (strcmp(name, ":authority") == 0 || strcmp(name, "host") == 0) {
        cwist_http_header_add(&req->headers, "host", value);
    } else if (strcmp(name, ":scheme") != 0 && name[0] != ':') {
        cwist_http_header_add(&req->headers, name, value);
    }
}

/**
 * @brief Decode an HPACK header block into request fields.
 */
static void h2_decode_header_block(cwist_http_request *req, const unsigned char *payload, size_t len) {
    size_t pos = 0;

    while (pos < len) {
        uint8_t b = payload[pos];
        uint32_t name_index = 0;
        char *name = NULL;
        char *value = NULL;

        if (b & 0x80) {
            uint32_t index = 0;
            if (h2_decode_integer(payload, len, &pos, 7, &index) != 0) break;
            const cwist_http2_static_header *entry = h2_static_header(index);
            if (entry && entry->name) h2_apply_header(req, entry->name, entry->value);
            continue;
        }

        if ((b & 0xE0) == 0x20) {
            uint32_t new_size = 0;
            if (h2_decode_integer(payload, len, &pos, 5, &new_size) != 0) break;
            continue;
        }

        if ((b & 0x40) == 0x40) {
            if (h2_decode_integer(payload, len, &pos, 6, &name_index) != 0) break;
        } else if ((b & 0xf0) == 0x00) {
            if (h2_decode_integer(payload, len, &pos, 4, &name_index) != 0) break;
        } else if ((b & 0xf0) == 0x10) {
            if (h2_decode_integer(payload, len, &pos, 4, &name_index) != 0) break;
        } else {
            break;
        }

        if (name_index > 0) {
            const cwist_http2_static_header *entry = h2_static_header(name_index);
            if (!entry || !entry->name) {
                /* Dynamic table reference or invalid index - skip value and continue */
                char *discard = h2_decode_string(payload, len, &pos);
                free(discard);
                continue;
            }
            name = strdup(entry->name);
        } else {
            name = h2_decode_string(payload, len, &pos);
        }
        value = h2_decode_string(payload, len, &pos);
        if (!name || !value) {
            free(name);
            free(value);
            break;
        }
        h2_apply_header(req, name, value);
        free(name);
        free(value);
    }
}

/* --- HPACK Response Encoder --- */

/**
 * @brief Encode an integer in HPACK format.
 */
static size_t h2_encode_integer(unsigned char *dst, size_t dst_cap, uint32_t value, uint8_t prefix_bits) {
    uint8_t mask = (uint8_t)((1u << prefix_bits) - 1u);
    unsigned char first = dst[0] & ~mask;
    if (value < mask) {
        dst[0] = first | (uint8_t)value;
        return 1;
    }
    dst[0] = first | mask;
    value -= mask;
    size_t i = 1;
    while (value >= 128) {
        if (i >= dst_cap) return 0;
        dst[i++] = (unsigned char)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    if (i >= dst_cap) return 0;
    dst[i++] = (unsigned char)value;
    return i;
}

/**
 * @brief Encode a literal string in HPACK format (no Huffman).
 */
static size_t h2_encode_string(unsigned char *dst, size_t dst_cap, const char *str) {
    size_t len = strlen(str);
    dst[0] = 0x00; /* literal, no huffman */
    size_t n = h2_encode_integer(dst, dst_cap, (uint32_t)len, 7);
    if (n == 0 || n + len > dst_cap) return 0;
    memcpy(dst + n, str, len);
    return n + len;
}

/**
 * @brief Find the static table index for a header name.
 */
static int h2_static_table_find_name(const char *name) {
    size_t count = sizeof(cwist_http2_static_table) / sizeof(cwist_http2_static_table[0]);
    for (size_t i = 1; i < count; ++i) {
        if (cwist_http2_static_table[i].name && strcasecmp(cwist_http2_static_table[i].name, name) == 0)
            return (int)i;
    }
    return 0;
}

/**
 * @brief Encode response headers into an HPACK header block.
 */
static size_t h2_encode_response_headers(cwist_http_response *res,
                                          unsigned char *dst, size_t dst_cap) {
    size_t pos = 0;

    /* :status */
    switch (res->status_code) {
        case 200: if (pos >= dst_cap) return 0; dst[pos++] = 0x88; break;
        case 204: if (pos >= dst_cap) return 0; dst[pos++] = 0x89; break;
        case 206: if (pos >= dst_cap) return 0; dst[pos++] = 0x8a; break;
        case 304: if (pos >= dst_cap) return 0; dst[pos++] = 0x8b; break;
        case 400: if (pos >= dst_cap) return 0; dst[pos++] = 0x8c; break;
        case 404: if (pos >= dst_cap) return 0; dst[pos++] = 0x8d; break;
        case 500: if (pos >= dst_cap) return 0; dst[pos++] = 0x8e; break;
        default: {
            char status_str[16];
            int status_len = snprintf(status_str, sizeof(status_str), "%d", res->status_code);
            if (pos + 1 > dst_cap) return 0;
            dst[pos] = 0x00; /* literal without indexing, literal name */
            size_t n = h2_encode_integer(dst + pos, dst_cap - pos, 0, 4);
            if (n == 0) return 0;
            pos += n;
            n = h2_encode_string(dst + pos, dst_cap - pos, ":status");
            if (n == 0) return 0;
            pos += n;
            n = h2_encode_string(dst + pos, dst_cap - pos, status_str);
            if (n == 0) return 0;
            pos += n;
            break;
        }
    }

    /* Auto content-length */
    size_t body_len = 0;
    if (res->use_file_stream) body_len = res->file_stream_len;
    else if (res->is_ptr_body) body_len = res->ptr_body_len;
    else if (res->body) body_len = res->body->size;

    if (!headers_have_content_length(res->headers)) {
        char cl_str[32];
        int cl_len = snprintf(cl_str, sizeof(cl_str), "%zu", body_len);
        int name_idx = h2_static_table_find_name("content-length");
        if (name_idx > 0) {
            if (pos + 1 > dst_cap) return 0;
            dst[pos] = 0x00; /* literal without indexing, indexed name */
            size_t n = h2_encode_integer(dst + pos, dst_cap - pos, (uint32_t)name_idx, 4);
            if (n == 0) return 0;
            pos += n;
        } else {
            if (pos + 1 > dst_cap) return 0;
            dst[pos] = 0x00;
            size_t n = h2_encode_integer(dst + pos, dst_cap - pos, 0, 4);
            if (n == 0) return 0;
            pos += n;
            n = h2_encode_string(dst + pos, dst_cap - pos, "content-length");
            if (n == 0) return 0;
            pos += n;
        }
        size_t n = h2_encode_string(dst + pos, dst_cap - pos, cl_str);
        if (n == 0) return 0;
        pos += n;
    }

    /* User headers */
    cwist_http_header_node *curr = res->headers;
    while (curr) {
        if (!curr->key || !curr->key->data || !curr->value || !curr->value->data) {
            curr = curr->next;
            continue;
        }
        /* Skip connection-specific headers (HTTP/2 forbids these in responses) */
        if (strcasecmp(curr->key->data, "connection") == 0 ||
            strcasecmp(curr->key->data, "keep-alive") == 0 ||
            strcasecmp(curr->key->data, "transfer-encoding") == 0 ||
            strcasecmp(curr->key->data, "upgrade") == 0) {
            curr = curr->next;
            continue;
        }

        char lower_name[256];
        size_t name_len = strlen(curr->key->data);
        if (name_len >= sizeof(lower_name)) name_len = sizeof(lower_name) - 1;
        for (size_t i = 0; i < name_len; ++i) {
            lower_name[i] = (char)tolower((unsigned char)curr->key->data[i]);
        }
        lower_name[name_len] = '\0';

        int name_idx = h2_static_table_find_name(lower_name);
        if (name_idx > 0) {
            if (pos + 1 > dst_cap) return 0;
            dst[pos] = 0x00; /* literal without indexing, indexed name */
            size_t n = h2_encode_integer(dst + pos, dst_cap - pos, (uint32_t)name_idx, 4);
            if (n == 0) return 0;
            pos += n;
        } else {
            if (pos + 1 > dst_cap) return 0;
            dst[pos] = 0x00;
            size_t n = h2_encode_integer(dst + pos, dst_cap - pos, 0, 4);
            if (n == 0) return 0;
            pos += n;
            n = h2_encode_string(dst + pos, dst_cap - pos, lower_name);
            if (n == 0) return 0;
            pos += n;
        }
        size_t n = h2_encode_string(dst + pos, dst_cap - pos, curr->value->data);
        if (n == 0) return 0;
        pos += n;

        curr = curr->next;
    }

    return pos;
}

/**
 * @brief Send a complete response (HEADERS + DATA) on the given stream.
 */
static int h2_send_response(cwist_https_connection *conn, uint32_t stream_id, cwist_http_response *res) {
    unsigned char header_block[8192];
    size_t block_len = h2_encode_response_headers(res, header_block, sizeof(header_block));
    if (block_len == 0) return -1;

    uint8_t header_flags = CWIST_HTTP2_FLAG_END_HEADERS;

    size_t body_len = 0;
    const unsigned char *body_data = NULL;
    if (res->use_file_stream) {
        body_len = res->file_stream_len;
    } else if (res->is_ptr_body) {
        body_len = res->ptr_body_len;
        body_data = (const unsigned char *)res->ptr_body;
    } else if (res->body) {
        body_len = res->body->size;
        body_data = (const unsigned char *)res->body->data;
    }

    if (body_len == 0) header_flags |= CWIST_HTTP2_FLAG_END_STREAM;

    if (h2_write_frame(conn, CWIST_HTTP2_FRAME_HEADERS, header_flags, stream_id,
                       header_block, (uint32_t)block_len) != 0) {
        return -1;
    }

    if (res->use_file_stream && res->file_stream_fd >= 0) {
        off_t offset = res->file_stream_offset;
        size_t remaining = res->file_stream_len;
        while (remaining > 0) {
            uint32_t chunk = (uint32_t)(remaining > CWIST_HTTP2_MAX_FRAME_SIZE ?
                                       CWIST_HTTP2_MAX_FRAME_SIZE : remaining);
            unsigned char *chunk_buf = (unsigned char *)malloc(chunk);
            if (!chunk_buf) return -1;
            ssize_t r = pread(res->file_stream_fd, chunk_buf, chunk, offset);
            if (r <= 0) { free(chunk_buf); return -1; }
            uint8_t flags = (remaining == (size_t)r) ? CWIST_HTTP2_FLAG_END_STREAM : 0;
            if (h2_write_frame(conn, CWIST_HTTP2_FRAME_DATA, flags, stream_id, chunk_buf, (uint32_t)r) != 0) {
                free(chunk_buf); return -1;
            }
            free(chunk_buf);
            offset += r;
            remaining -= (size_t)r;
        }
    } else {
        size_t sent = 0;
        while (sent < body_len) {
            size_t remaining = body_len - sent;
            uint32_t chunk = (uint32_t)(remaining > CWIST_HTTP2_MAX_FRAME_SIZE ?
                                       CWIST_HTTP2_MAX_FRAME_SIZE : remaining);
            uint8_t flags = (sent + chunk == body_len) ? CWIST_HTTP2_FLAG_END_STREAM : 0;
            if (h2_write_frame(conn, CWIST_HTTP2_FRAME_DATA, flags, stream_id,
                               body_data + sent, chunk) != 0) {
                return -1;
            }
            sent += chunk;
        }
    }
    return 0;
}

/**
 * @brief Verifies the mandatory HTTP/2 connection preface.
 *
 * @param conn Pointer to the connection to read from.
 * @return 0 on success (preface matches), -1 on failure.
 */
static int cwist_http2_verify_preface(cwist_https_connection *conn) {
    char buffer[CWIST_HTTP2_CONNECTION_PREFACE_LEN];
    int offset = 0;
    while (offset < CWIST_HTTP2_CONNECTION_PREFACE_LEN) {
        int n = h2_read(conn, buffer + offset, CWIST_HTTP2_CONNECTION_PREFACE_LEN - offset);
        if (n <= 0) return -1;
        offset += n;
    }

    if (memcmp(buffer, CWIST_HTTP2_CONNECTION_PREFACE, CWIST_HTTP2_CONNECTION_PREFACE_LEN) != 0) {
        return -1;
    }

    return 0;
}

/**
 * @brief Serves an HTTP/2 connection.
 *
 * Processes HTTP/2 frames, decodes HPACK headers (minimal support for testing),
 * triggers the appropriate user-defined request handler, and multiplexes
 * the response back to the client. Works with both TLS (h2) and cleartext (h2c).
 *
 * @param conn Pointer to the HTTPS connection structure (ssl can be NULL for h2c).
 * @param user_ctx Opaque user context to pass to the handler.
 * @param handler Function pointer to the user's HTTP request handler.
 * @return cwist_error_t Structure indicating success or failure.
 */
cwist_error_t cwist_http2_serve_connection(
    cwist_https_connection *conn,
    void *user_ctx,
    cwist_http2_request_handler_func handler
) {
    cwist_error_t result;

    /* 1. Validation */
    if (!conn || !handler) {
        result = make_error(CWIST_ERR_INT16);
        result.error.err_i16 = -1;
        return result;
    }

    /* 2. Verify HTTP/2 Connection Preface */
    if (cwist_http2_verify_preface(conn) != 0) {
        result = make_error(CWIST_ERR_INT16);
        result.error.err_i16 = -1;
        return result;
    }

    unsigned char settings[6] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x00}; /* SETTINGS_HEADER_TABLE_SIZE = 0 */
    if (h2_write_frame(conn, CWIST_HTTP2_FRAME_SETTINGS, 0, 0, settings, sizeof(settings)) != 0) {
        result = make_error(CWIST_ERR_INT16);
        result.error.err_i16 = -1;
        return result;
    }

    /* 3. Main Event Loop */
    bool connected = true;
    cwist_http_request *pending_req = NULL;
    uint32_t pending_stream_id = 0;
    while (connected && atomic_load(&g_cwist_running)) {
        unsigned char hdr[9];
        int n = 0;
        int offset = 0;
        // Read 9-byte frame header
        while (offset < 9) {
            n = h2_read(conn, hdr + offset, 9 - offset);
            if (n <= 0) { connected = false; break; }
            offset += n;
        }
        if (!connected) break;

        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | hdr[2];
        uint8_t type = hdr[3];
        uint32_t stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 8) | hdr[8];

        if (len > CWIST_HTTP2_MAX_FRAME_SIZE) {
            connected = false;
            break;
        }

        unsigned char *payload = NULL;
        if (len > 0) {
            payload = malloc(len);
            int off = 0;
            while (off < (int)len) {
                int r = h2_read(conn, payload + off, len - off);
                if (r <= 0) { connected = false; break; }
                off += r;
            }
        }
        if (!connected) { free(payload); break; }

        if (type == CWIST_HTTP2_FRAME_SETTINGS) {
            if ((hdr[4] & CWIST_HTTP2_FLAG_ACK) == 0) {
                h2_write_frame(conn, CWIST_HTTP2_FRAME_SETTINGS, CWIST_HTTP2_FLAG_ACK, 0, NULL, 0);
            }
        } else if (type == CWIST_HTTP2_FRAME_HEADERS && stream_id != 0) {
            cwist_http_request *req = cwist_http_request_create();
            if (!req) { free(payload); break; }
            cwist_sstring_assign(req->version, "HTTP/2");
            req->stream_id = stream_id;
            req->private_data = conn;

            size_t block_offset = 0;
            size_t block_len = len;
            if ((hdr[4] & CWIST_HTTP2_FLAG_PADDED) != 0 && block_len > 0) {
                uint8_t pad_len = payload[block_offset++];
                block_len--;
                if (pad_len <= block_len) block_len -= pad_len;
            }
            if ((hdr[4] & CWIST_HTTP2_FLAG_PRIORITY) != 0 && block_len >= 5) {
                block_offset += 5;
                block_len -= 5;
            }
            if (payload && block_offset <= len) {
                h2_decode_header_block(req, payload + block_offset, block_len);
            }

            if (hdr[4] & CWIST_HTTP2_FLAG_END_STREAM) {
                cwist_http_response *res = cwist_http_response_create();
                if (res) {
                    handler(user_ctx, req, res);
                    if (conn->http3_enabled) {
                        struct sockaddr_storage ss;
                        socklen_t ss_len = sizeof(ss);
                        int port = 443;
                        if (getsockname(conn->fd, (struct sockaddr *)&ss, &ss_len) == 0) {
                            if (ss.ss_family == AF_INET) {
                                port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
                            } else if (ss.ss_family == AF_INET6) {
                                port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
                            }
                        }
                        char alt_svc[64];
                        snprintf(alt_svc, sizeof(alt_svc), "h3=\":%d\"; ma=86400", port);
                        cwist_http_header_add(&res->headers, "Alt-Svc", alt_svc);
                    }
                    if (h2_send_response(conn, stream_id, res) != 0) connected = false;
                    cwist_http_response_destroy(res);
                }
                cwist_http_request_destroy(req);
            } else {
                pending_req = req;
                pending_stream_id = stream_id;
            }
        } else if (type == CWIST_HTTP2_FRAME_DATA && stream_id != 0) {
            if (pending_req && pending_stream_id == stream_id) {
                if (payload && len > 0) {
                    cwist_sstring_append_len(pending_req->body, (const char *)payload, len);
                }
                if (hdr[4] & CWIST_HTTP2_FLAG_END_STREAM) {
                    cwist_http_response *res = cwist_http_response_create();
                    if (res) {
                        handler(user_ctx, pending_req, res);
                        if (conn->http3_enabled) {
                            struct sockaddr_storage ss;
                            socklen_t ss_len = sizeof(ss);
                            int port = 443;
                            if (getsockname(conn->fd, (struct sockaddr *)&ss, &ss_len) == 0) {
                                if (ss.ss_family == AF_INET) {
                                    port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
                                } else if (ss.ss_family == AF_INET6) {
                                    port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
                                }
                            }
                            char alt_svc[64];
                            snprintf(alt_svc, sizeof(alt_svc), "h3=\":%d\"; ma=86400", port);
                            cwist_http_header_add(&res->headers, "Alt-Svc", alt_svc);
                        }
                        if (h2_send_response(conn, stream_id, res) != 0) connected = false;
                        cwist_http_response_destroy(res);
                    }
                    cwist_http_request_destroy(pending_req);
                    pending_req = NULL;
                    pending_stream_id = 0;
                }
            }
        }

        free(payload);
    }

    result = make_error(CWIST_ERR_INT16);
    result.error.err_i16 = 0;
    return result;
}

/* ------------------------------------------------------------------ */
/* HTTP/2 Server Push                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Push a resource to the client over HTTP/2 Server Push.
 *
 * Sends a PUSH_PROMISE frame on the original request stream, then delivers
 * the pushed response headers and body on a newly allocated server-initiated
 * stream.  The caller should ensure HTTP/2 Server Push is enabled on the
 * connection (via SETTINGS_ENABLE_PUSH).
 *
 * @param req            The current HTTP/2 request (must have stream_id and
 *                       private_data populated by the HTTP/2 layer).
 * @param path           The resource path to push (e.g., "/style.css").
 * @param content_type   Optional Content-Type header value (may be NULL).
 * @param data           Response body bytes (may be NULL).
 * @param data_len       Length of @p data.
 * @return 0 on success, -1 on failure.
 */
int cwist_http2_push_resource(cwist_http_request *req,
                              const char *path,
                              const char *content_type,
                              const unsigned char *data,
                              size_t data_len) {
    if (!req || !req->private_data || !path) return -1;

    cwist_https_connection *conn = (cwist_https_connection *)req->private_data;
    uint32_t original_stream_id = req->stream_id;

    /* Find :authority from the original request headers */
    const char *authority = NULL;
    cwist_http_header_node *h = req->headers;
    while (h) {
        if (h->key && h->key->data && strcasecmp(h->key->data, "host") == 0) {
            authority = h->value ? h->value->data : NULL;
            break;
        }
        h = h->next;
    }
    if (!authority) return -1;

    /* Allocate a new server-initiated stream ID (even number) */
    static _Atomic uint32_t next_server_stream = 2;
    uint32_t promised_stream_id = atomic_fetch_add(&next_server_stream, 2);

    /* Encode PUSH_PROMISE pseudo-headers (:method, :scheme, :authority, :path) */
    unsigned char header_block[4096];
    size_t pos = 0;

    /* :method = GET → static table index 2 (0x82) */
    if (pos >= sizeof(header_block)) return -1;
    header_block[pos++] = 0x82;

    /* :scheme = https → static table index 7 (0x87) */
    if (pos >= sizeof(header_block)) return -1;
    header_block[pos++] = 0x87;

    /* :authority = host (literal without indexing) */
    if (pos + 1 > sizeof(header_block)) return -1;
    header_block[pos] = 0x00;
    size_t n = h2_encode_integer(header_block + pos, sizeof(header_block) - pos, 0, 4);
    if (n == 0) return -1;
    pos += n;
    n = h2_encode_string(header_block + pos, sizeof(header_block) - pos, ":authority");
    if (n == 0) return -1;
    pos += n;
    n = h2_encode_string(header_block + pos, sizeof(header_block) - pos, authority);
    if (n == 0) return -1;
    pos += n;

    /* :path = path (literal without indexing) */
    if (pos + 1 > sizeof(header_block)) return -1;
    header_block[pos] = 0x00;
    n = h2_encode_integer(header_block + pos, sizeof(header_block) - pos, 0, 4);
    if (n == 0) return -1;
    pos += n;
    n = h2_encode_string(header_block + pos, sizeof(header_block) - pos, ":path");
    if (n == 0) return -1;
    pos += n;
    n = h2_encode_string(header_block + pos, sizeof(header_block) - pos, path);
    if (n == 0) return -1;
    pos += n;

    /* Build PUSH_PROMISE frame payload: promised_stream_id (4 bytes) + header_block */
    unsigned char pp_payload[4096 + 4];
    pp_payload[0] = (unsigned char)((promised_stream_id >> 24) & 0x7f);
    pp_payload[1] = (unsigned char)((promised_stream_id >> 16) & 0xff);
    pp_payload[2] = (unsigned char)((promised_stream_id >> 8) & 0xff);
    pp_payload[3] = (unsigned char)(promised_stream_id & 0xff);
    memcpy(pp_payload + 4, header_block, pos);

    /* Send PUSH_PROMISE frame */
    if (h2_write_frame(conn, 0x05, CWIST_HTTP2_FLAG_END_HEADERS,
                       original_stream_id, pp_payload, (uint32_t)(pos + 4)) != 0) {
        return -1;
    }

    /* Build and send the pushed response on the promised stream */
    cwist_http_response *res = cwist_http_response_create();
    if (!res) return -1;
    res->status_code = CWIST_HTTP_OK;
    if (content_type) {
        cwist_http_header_add(&res->headers, "content-type", content_type);
    }
    if (data && data_len > 0) {
        cwist_sstring_assign_len(res->body, (const char *)data, data_len);
    }

    int ret = h2_send_response(conn, promised_stream_id, res);
    cwist_http_response_destroy(res);
    return ret;
}
