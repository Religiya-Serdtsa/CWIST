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
#define CWIST_HTTP2_FRAME_RST_STREAM 0x03
#define CWIST_HTTP2_FRAME_SETTINGS 0x04
#define CWIST_HTTP2_FRAME_PING 0x06
#define CWIST_HTTP2_FRAME_GOAWAY 0x07
#define CWIST_HTTP2_FRAME_WINDOW_UPDATE 0x08
#define CWIST_HTTP2_FRAME_CONTINUATION 0x09
#define CWIST_HTTP2_FRAME_PUSH_PROMISE 0x05

#define H2_ERR_NO_ERROR           0x0
#define H2_ERR_PROTOCOL_ERROR     0x1
#define H2_ERR_FLOW_CONTROL_ERROR 0x3
#define H2_ERR_SETTINGS_TIMEOUT   0x4
#define H2_ERR_STREAM_CLOSED      0x5
#define H2_ERR_FRAME_SIZE_ERROR   0x6
#define H2_ERR_REFUSED_STREAM     0x7
#define H2_ERR_COMPRESSION_ERROR  0x9

/* --- Stream & Connection State --- */

typedef struct h2_stream {
    uint32_t stream_id;
    cwist_http_request *req;
    int32_t send_window;   /* peer-advertised; bytes we may send */
    int32_t recv_window;   /* our window; bytes peer may send */
    struct h2_stream *next;
} h2_stream;

typedef struct {
    cwist_https_connection *conn;
    h2_stream *streams;
    uint32_t peer_max_frame_size;
    uint32_t peer_initial_window_size;
    uint32_t peer_max_concurrent_streams;
    int32_t conn_send_window;
    int32_t conn_recv_window;
    unsigned char *cont_buf;
    size_t cont_len;
    size_t cont_cap;
    uint32_t cont_stream_id;
    bool expecting_continuation;
    bool cont_end_stream;
    uint32_t last_processed_stream_id;
} h2_conn;

static void h2_conn_init(h2_conn *hc, cwist_https_connection *conn) {
    memset(hc, 0, sizeof(*hc));
    hc->conn = conn;
    hc->peer_max_frame_size = CWIST_HTTP2_MAX_FRAME_SIZE;
    hc->peer_initial_window_size = 65535;
    hc->conn_send_window = 65535;
    hc->conn_recv_window = 65535;
    hc->cont_end_stream = false;
}

static void h2_conn_destroy(h2_conn *hc) {
    h2_stream *s = hc->streams;
    while (s) {
        h2_stream *next = s->next;
        if (s->req) cwist_http_request_destroy(s->req);
        cwist_free(s);
        s = next;
    }
    cwist_free(hc->cont_buf);
}

static h2_stream *h2_stream_find(h2_conn *hc, uint32_t stream_id) {
    h2_stream *s = hc->streams;
    while (s) {
        if (s->stream_id == stream_id) return s;
        s = s->next;
    }
    return NULL;
}

static h2_stream *h2_stream_create(h2_conn *hc, uint32_t stream_id) {
    h2_stream *s = (h2_stream *)cwist_alloc(sizeof(*s));
    if (!s) return NULL;
    s->stream_id = stream_id;
    s->send_window = (int32_t)hc->peer_initial_window_size;
    s->recv_window = 65535;
    s->next = hc->streams;
    hc->streams = s;
    return s;
}

static void h2_stream_remove(h2_conn *hc, uint32_t stream_id) {
    h2_stream **pp = &hc->streams;
    while (*pp) {
        h2_stream *s = *pp;
        if (s->stream_id == stream_id) {
            *pp = s->next;
            if (s->req) cwist_http_request_destroy(s->req);
            cwist_free(s);
            return;
        }
        pp = &s->next;
    }
}

/* --- Static Header Table --- */

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

/* --- I/O Helpers --- */

static int h2_read(cwist_https_connection *conn, void *buf, int len) {
    if (conn->ssl) return SSL_read(conn->ssl, buf, len);
    return read(conn->fd, buf, len);
}

static int h2_write(cwist_https_connection *conn, const void *buf, int len) {
    if (conn->ssl) return SSL_write(conn->ssl, buf, len);
    return write(conn->fd, buf, len);
}

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

/* --- GOAWAY & WINDOW_UPDATE Helpers --- */

static int h2_send_goaway(h2_conn *hc, uint32_t last_stream_id, uint32_t error_code) {
    unsigned char payload[8];
    payload[0] = (unsigned char)((last_stream_id >> 24) & 0x7f);
    payload[1] = (unsigned char)((last_stream_id >> 16) & 0xff);
    payload[2] = (unsigned char)((last_stream_id >> 8) & 0xff);
    payload[3] = (unsigned char)(last_stream_id & 0xff);
    payload[4] = (unsigned char)((error_code >> 24) & 0xff);
    payload[5] = (unsigned char)((error_code >> 16) & 0xff);
    payload[6] = (unsigned char)((error_code >> 8) & 0xff);
    payload[7] = (unsigned char)(error_code & 0xff);
    return h2_write_frame(hc->conn, CWIST_HTTP2_FRAME_GOAWAY, 0, 0, payload, 8);
}

static int h2_send_window_update(h2_conn *hc, uint32_t stream_id, int32_t increment) {
    unsigned char payload[4];
    payload[0] = (unsigned char)((increment >> 24) & 0x7f);
    payload[1] = (unsigned char)((increment >> 16) & 0xff);
    payload[2] = (unsigned char)((increment >> 8) & 0xff);
    payload[3] = (unsigned char)(increment & 0xff);
    return h2_write_frame(hc->conn, CWIST_HTTP2_FRAME_WINDOW_UPDATE, 0, stream_id, payload, 4);
}

static int h2_auto_window_update(h2_conn *hc, h2_stream *s) {
    if (hc->conn_recv_window < 32768) {
        int32_t inc = 65535 - hc->conn_recv_window;
        if (inc > 0 && h2_send_window_update(hc, 0, inc) == 0) {
            hc->conn_recv_window += inc;
        }
    }
    if (s && s->recv_window < 32768) {
        int32_t inc = 65535 - s->recv_window;
        if (inc > 0 && h2_send_window_update(hc, s->stream_id, inc) == 0) {
            s->recv_window += inc;
        }
    }
    return 0;
}

/* --- HPACK Integer Decoder --- */

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
    char *out = (char *)cwist_alloc(cap);
    if (!out) return NULL;
    size_t out_pos = 0;

    h2_huffman_node *node = h2_huffman_root;
    for (size_t i = 0; i < src_len; ++i) {
        unsigned char byte = src[i];
        for (int b = 7; b >= 0; --b) {
            int bit = (byte >> b) & 1;
            node = node->child[bit];
            if (!node) { cwist_free(out); return NULL; }
            if (node->is_terminal) {
                if (node->symbol == 256) {
                    cwist_free(out);
                    return NULL;
                }
                if (out_pos + 1 >= cap) {
                    cap *= 2;
                    char *tmp = (char *)cwist_realloc(out, cap);
                    if (!tmp) { cwist_free(out); return NULL; }
                    out = tmp;
                }
                out[out_pos++] = (char)node->symbol;
                node = h2_huffman_root;
            }
        }
    }

    if (node != h2_huffman_root) {
        h2_huffman_node *check = node;
        while (check && !check->is_terminal) {
            check = check->child[1];
        }
        if (!check || check->symbol != 256) {
            cwist_free(out);
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

    char *out = (char *)cwist_alloc((size_t)str_len + 1);
    if (!out) return NULL;
    memcpy(out, buf + *pos, str_len);
    out[str_len] = '\0';
    *pos += str_len;
    return out;
}

static const cwist_http2_static_header *h2_static_header(uint32_t index) {
    size_t count = sizeof(cwist_http2_static_table) / sizeof(cwist_http2_static_table[0]);
    if (index == 0 || index >= count) return NULL;
    return &cwist_http2_static_table[index];
}

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
                char *discard = h2_decode_string(payload, len, &pos);
                cwist_free(discard);
                continue;
            }
            name = strdup(entry->name);
        } else {
            name = h2_decode_string(payload, len, &pos);
        }
        value = h2_decode_string(payload, len, &pos);
        if (!name || !value) {
            cwist_free(name);
            cwist_free(value);
            break;
        }
        h2_apply_header(req, name, value);
        free(name);
        free(value);
    }
}

/* --- HPACK Response Encoder --- */

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

static size_t h2_encode_string(unsigned char *dst, size_t dst_cap, const char *str) {
    size_t len = strlen(str);
    dst[0] = 0x00;
    size_t n = h2_encode_integer(dst, dst_cap, (uint32_t)len, 7);
    if (n == 0 || n + len > dst_cap) return 0;
    memcpy(dst + n, str, len);
    return n + len;
}

static int h2_static_table_find_name(const char *name) {
    size_t count = sizeof(cwist_http2_static_table) / sizeof(cwist_http2_static_table[0]);
    for (size_t i = 1; i < count; ++i) {
        if (cwist_http2_static_table[i].name && strcasecmp(cwist_http2_static_table[i].name, name) == 0)
            return (int)i;
    }
    return 0;
}

static size_t h2_encode_response_headers(cwist_http_response *res,
                                          unsigned char *dst, size_t dst_cap) {
    size_t pos = 0;

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
            snprintf(status_str, sizeof(status_str), "%d", res->status_code);
            if (pos + 1 > dst_cap) return 0;
            dst[pos] = 0x00;
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

    size_t body_len = 0;
    if (res->use_file_stream) body_len = res->file_stream_len;
    else if (res->is_ptr_body) body_len = res->ptr_body_len;
    else if (res->body) body_len = res->body->size;

    if (!headers_have_content_length(res->headers)) {
        char cl_str[32];
        snprintf(cl_str, sizeof(cl_str), "%zu", body_len);
        int name_idx = h2_static_table_find_name("content-length");
        if (name_idx > 0) {
            if (pos + 1 > dst_cap) return 0;
            dst[pos] = 0x00;
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

    cwist_http_header_node *curr = res->headers;
    while (curr) {
        if (!curr->key || !curr->key->data || !curr->value || !curr->value->data) {
            curr = curr->next;
            continue;
        }
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
            dst[pos] = 0x00;
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

/* --- Response Senders --- */

static int h2_send_response_raw(cwist_https_connection *conn, uint32_t stream_id,
                                 cwist_http_response *res, uint32_t max_frame_size) {
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
            uint32_t chunk = (uint32_t)(remaining > max_frame_size ? max_frame_size : remaining);
            unsigned char *chunk_buf = (unsigned char *)cwist_alloc(chunk);
            if (!chunk_buf) return -1;
            ssize_t r = pread(res->file_stream_fd, chunk_buf, chunk, offset);
            if (r <= 0) { cwist_free(chunk_buf); return -1; }
            uint8_t flags = (remaining == (size_t)r) ? CWIST_HTTP2_FLAG_END_STREAM : 0;
            if (h2_write_frame(conn, CWIST_HTTP2_FRAME_DATA, flags, stream_id, chunk_buf, (uint32_t)r) != 0) {
                cwist_free(chunk_buf); return -1;
            }
            cwist_free(chunk_buf);
            offset += r;
            remaining -= (size_t)r;
        }
    } else {
        size_t sent = 0;
        while (sent < body_len) {
            size_t remaining = body_len - sent;
            uint32_t chunk = (uint32_t)(remaining > max_frame_size ? max_frame_size : remaining);
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

/* Kept for external compatibility if any caller relies on the old signature */
__attribute__((unused))
static int h2_send_response(cwist_https_connection *conn, uint32_t stream_id, cwist_http_response *res) {
    return h2_send_response_raw(conn, stream_id, res, CWIST_HTTP2_MAX_FRAME_SIZE);
}

static int h2_read_all(cwist_https_connection *conn, void *buf, int len) {
    int total = 0;
    while (total < len) {
        int n = h2_read(conn, (char *)buf + total, len - total);
        if (n < 0) return -1;
        if (n == 0) {
            struct timespec ts = {0, 1000000}; // 1ms
            nanosleep(&ts, NULL);
            continue;
        }
        total += n;
    }
    return total;
}

static void h2_process_incoming_frames_nonblocking(h2_conn *hc, h2_stream *s) {
    struct pollfd pfd;
    pfd.fd = hc->conn->fd;
    pfd.events = POLLIN;

    while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        unsigned char hdr[9];
        int n = h2_read_all(hc->conn, hdr, 9);
        if (n != 9) break;

        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | hdr[2];
        uint8_t type = hdr[3];
        uint8_t flags = hdr[4];
        uint32_t stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) |
                             ((uint32_t)hdr[6] << 16) |
                             ((uint32_t)hdr[7] << 8) |
                             hdr[8];

        unsigned char *payload = NULL;
        if (len > 0) {
            payload = (unsigned char *)cwist_alloc(len);
            if (!payload) break;
            int r = h2_read_all(hc->conn, payload, len);
            if (r != (int)len) {
                cwist_free(payload);
                break;
            }
        }

        if (type == 0x08) { // WINDOW_UPDATE
            if (len == 4) {
                int32_t increment = (((int32_t)payload[0] & 0x7f) << 24) |
                                    ((int32_t)payload[1] << 16) |
                                    ((int32_t)payload[2] << 8) |
                                    (int32_t)payload[3];
                if (increment > 0) {
                    if (stream_id == 0) {
                        hc->conn_send_window += increment;
                    } else if (s && stream_id == s->stream_id) {
                        s->send_window += increment;
                    } else {
                        h2_stream *other = h2_stream_find(hc, stream_id);
                        if (other) other->send_window += increment;
                    }
                }
            }
        } else if (type == 0x06) { // PING
            if ((flags & 0x01) == 0 && len == 8) { // ACK flag is 0x01
                h2_write_frame(hc->conn, 0x06, 0x01, 0, payload, 8);
            }
        } else if (type == 0x03) { // RST_STREAM
            if (s && stream_id == s->stream_id) {
                s->send_window = -999; // abort code
            }
        }
        cwist_free(payload);
    }
}

static int h2_send_response_hc(h2_conn *hc, uint32_t stream_id, cwist_http_response *res) {
    unsigned char header_block[8192];
    size_t block_len = h2_encode_response_headers(res, header_block, sizeof(header_block));
    if (block_len == 0) return -1;

    h2_stream *s = h2_stream_find(hc, stream_id);

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

    if (h2_write_frame(hc->conn, CWIST_HTTP2_FRAME_HEADERS, header_flags, stream_id,
                       header_block, (uint32_t)block_len) != 0) {
        return -1;
    }

    uint32_t max_frame = hc->peer_max_frame_size;
    if (max_frame == 0) max_frame = CWIST_HTTP2_MAX_FRAME_SIZE;

    if (res->use_file_stream && res->file_stream_fd >= 0) {
        off_t offset = res->file_stream_offset;
        size_t remaining = res->file_stream_len;
        while (remaining > 0) {
            while (hc->conn_send_window <= 0 || (s && s->send_window <= 0)) {
                h2_process_incoming_frames_nonblocking(hc, s);
                if (s && s->send_window == -999) return -1; // stream reset
                if (hc->conn_send_window <= 0 || (s && s->send_window <= 0)) {
                    struct timespec ts = {0, 2000000}; // 2ms sleep
                    nanosleep(&ts, NULL);
                }
            }

            uint32_t chunk = (uint32_t)(remaining > max_frame ? max_frame : remaining);
            uint32_t allowed = chunk;
            if ((int32_t)allowed > hc->conn_send_window) allowed = (uint32_t)hc->conn_send_window;
            if (s && (int32_t)allowed > s->send_window) allowed = (uint32_t)s->send_window;

            unsigned char *chunk_buf = (unsigned char *)cwist_alloc(allowed);
            if (!chunk_buf) return -1;
            ssize_t r = pread(res->file_stream_fd, chunk_buf, allowed, offset);
            if (r <= 0) { free(chunk_buf); return -1; }
            uint8_t flags = (remaining == (size_t)r) ? CWIST_HTTP2_FLAG_END_STREAM : 0;
            if (h2_write_frame(hc->conn, CWIST_HTTP2_FRAME_DATA, flags, stream_id, chunk_buf, (uint32_t)r) != 0) {
                free(chunk_buf); return -1;
            }
            free(chunk_buf);
            hc->conn_send_window -= (int32_t)r;
            if (s) s->send_window -= (int32_t)r;
            offset += r;
            remaining -= (size_t)r;
        }
    } else {
        size_t sent = 0;
        while (sent < body_len) {
            while (hc->conn_send_window <= 0 || (s && s->send_window <= 0)) {
                h2_process_incoming_frames_nonblocking(hc, s);
                if (s && s->send_window == -999) return -1; // stream reset
                if (hc->conn_send_window <= 0 || (s && s->send_window <= 0)) {
                    struct timespec ts = {0, 2000000}; // 2ms sleep
                    nanosleep(&ts, NULL);
                }
            }

            size_t remaining = body_len - sent;
            uint32_t chunk = (uint32_t)(remaining > max_frame ? max_frame : remaining);
            uint32_t allowed = chunk;
            if ((int32_t)allowed > hc->conn_send_window) allowed = (uint32_t)hc->conn_send_window;
            if (s && (int32_t)allowed > s->send_window) allowed = (uint32_t)s->send_window;

            uint8_t flags = (sent + allowed == body_len) ? CWIST_HTTP2_FLAG_END_STREAM : 0;
            if (h2_write_frame(hc->conn, CWIST_HTTP2_FRAME_DATA, flags, stream_id,
                               body_data + sent, allowed) != 0) {
                return -1;
            }
            hc->conn_send_window -= (int32_t)allowed;
            if (s) s->send_window -= (int32_t)allowed;
            sent += allowed;
        }
    }
    return 0;
}

/* --- CONTINUATION & Header Assembly --- */

static int h2_begin_headers(h2_conn *hc, uint32_t stream_id,
                            const unsigned char *payload, size_t len,
                            bool end_headers, bool end_stream) {
    if (hc->expecting_continuation) {
        return -1; /* PROTOCOL_ERROR: HEADERS while expecting CONTINUATION */
    }

    size_t block_offset = 0;
    size_t block_len = len;

    if (end_headers) {
        h2_stream *s = h2_stream_find(hc, stream_id);
        if (!s) {
            s = h2_stream_create(hc, stream_id);
            if (!s) return -1;
            s->req = cwist_http_request_create();
            if (!s->req) {
                h2_stream_remove(hc, stream_id);
                return -1;
            }
            cwist_sstring_assign(s->req->version, "HTTP/2");
            s->req->stream_id = stream_id;
            s->req->private_data = hc->conn;
        }
        if (payload && block_len > 0) {
            h2_decode_header_block(s->req, payload + block_offset, block_len);
        }
        return 0;
    }

    /* Need continuation */
    hc->expecting_continuation = true;
    hc->cont_stream_id = stream_id;
    hc->cont_end_stream = end_stream;
    hc->cont_len = block_len;
    hc->cont_cap = block_len < 1024 ? 1024 : block_len * 2;
    hc->cont_buf = (unsigned char *)cwist_alloc(hc->cont_cap);
    if (!hc->cont_buf) {
        hc->expecting_continuation = false;
        return -1;
    }
    if (payload && block_len > 0) {
        memcpy(hc->cont_buf, payload + block_offset, block_len);
    }
    return 0;
}

static int h2_handle_continuation(h2_conn *hc, uint32_t stream_id,
                                  const unsigned char *payload, size_t len,
                                  bool end_headers) {
    if (!hc->expecting_continuation || hc->cont_stream_id != stream_id) {
        return -1; /* PROTOCOL_ERROR */
    }

    if (len > 0) {
        if (hc->cont_len + len > hc->cont_cap) {
            size_t new_cap = hc->cont_cap * 2;
            while (new_cap < hc->cont_len + len) new_cap *= 2;
            unsigned char *tmp = (unsigned char *)cwist_realloc(hc->cont_buf, new_cap);
            if (!tmp) return -1;
            hc->cont_buf = tmp;
            hc->cont_cap = new_cap;
        }
        memcpy(hc->cont_buf + hc->cont_len, payload, len);
        hc->cont_len += len;
    }

    if (end_headers) {
        h2_stream *s = h2_stream_find(hc, hc->cont_stream_id);
        if (!s) {
            s = h2_stream_create(hc, hc->cont_stream_id);
            if (!s) {
                hc->expecting_continuation = false;
                return -1;
            }
            s->req = cwist_http_request_create();
            if (!s->req) {
                h2_stream_remove(hc, hc->cont_stream_id);
                hc->expecting_continuation = false;
                return -1;
            }
            cwist_sstring_assign(s->req->version, "HTTP/2");
            s->req->stream_id = hc->cont_stream_id;
            s->req->private_data = hc->conn;
        }
        if (hc->cont_buf && hc->cont_len > 0) {
            h2_decode_header_block(s->req, hc->cont_buf, hc->cont_len);
        }
        hc->expecting_continuation = false;
        hc->cont_len = 0;
        hc->cont_stream_id = 0;
    }
    return 0;
}

/* --- SETTINGS Parser --- */

static int h2_handle_settings(h2_conn *hc, const unsigned char *payload, size_t len, bool ack) {
    if (ack) {
        return 0;
    }

    if (len % 6 != 0) {
        return -1; /* FRAME_SIZE_ERROR */
    }

    for (size_t i = 0; i + 6 <= len; i += 6) {
        uint16_t id = ((uint16_t)payload[i] << 8) | payload[i + 1];
        uint32_t value = ((uint32_t)payload[i + 2] << 24) |
                         ((uint32_t)payload[i + 3] << 16) |
                         ((uint32_t)payload[i + 4] << 8) |
                         (uint32_t)payload[i + 5];
        switch (id) {
            case 0x1: /* SETTINGS_HEADER_TABLE_SIZE */
                break;
            case 0x2: /* SETTINGS_ENABLE_PUSH */
                if (value > 1) return -1; /* PROTOCOL_ERROR */
                break;
            case 0x3: /* SETTINGS_MAX_CONCURRENT_STREAMS */
                hc->peer_max_concurrent_streams = value;
                break;
            case 0x4: /* SETTINGS_INITIAL_WINDOW_SIZE */
                if (value > 2147483647U) return -1; /* FLOW_CONTROL_ERROR */
                {
                    int32_t delta = (int32_t)value - (int32_t)hc->peer_initial_window_size;
                    hc->peer_initial_window_size = value;
                    h2_stream *s = hc->streams;
                    while (s) {
                        s->send_window += delta;
                        s = s->next;
                    }
                }
                break;
            case 0x5: /* SETTINGS_MAX_FRAME_SIZE */
                if (value < 16384 || value > 16777215) return -1; /* PROTOCOL_ERROR */
                hc->peer_max_frame_size = value;
                break;
            case 0x6: /* SETTINGS_MAX_HEADER_LIST_SIZE */
                break;
        }
    }
    return 0;
}

/* --- WINDOW_UPDATE Parser --- */

static int h2_handle_window_update(h2_conn *hc, uint32_t stream_id,
                                   const unsigned char *payload, size_t len) {
    if (len != 4) return -1;

    int32_t increment = (int32_t)(
        (((uint32_t)payload[0] & 0x7f) << 24) |
        ((uint32_t)payload[1] << 16) |
        ((uint32_t)payload[2] << 8) |
        (uint32_t)payload[3]
    );
    if (increment <= 0) return -1;

    if (stream_id == 0) {
        hc->conn_send_window += increment;
        if (hc->conn_send_window > 2147483647 || hc->conn_send_window < 0) return -1;
    } else {
        h2_stream *s = h2_stream_find(hc, stream_id);
        if (!s) {
            /* RFC 7540: WINDOW_UPDATE on closed stream is a STREAM_CLOSED error
             * or may be ignored depending on state. We ignore for leniency. */
            return 0;
        }
        s->send_window += increment;
        if (s->send_window > 2147483647 || s->send_window < 0) return -1;
    }
    return 0;
}

/* --- Preface Verification --- */

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

/* --- Alt-Svc Injection Helper --- */

static void h2_inject_alt_svc(cwist_https_connection *conn, cwist_http_response *res) {
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
}

/* --- Connection Dispatcher --- */

cwist_error_t cwist_http2_serve_connection(
    cwist_https_connection *conn,
    void *user_ctx,
    cwist_http2_request_handler_func handler
) {
    cwist_error_t result;

    if (!conn || !handler) {
        result = make_error(CWIST_ERR_INT16);
        result.error.err_i16 = -1;
        return result;
    }

    if (cwist_http2_verify_preface(conn) != 0) {
        result = make_error(CWIST_ERR_INT16);
        result.error.err_i16 = -1;
        return result;
    }

    h2_conn hc;
    h2_conn_init(&hc, conn);

    unsigned char settings[12] = {
        0x00, 0x01, 0x00, 0x00, 0x10, 0x00, // SETTINGS_HEADER_TABLE_SIZE = 4096
        0x00, 0x04, 0x7f, 0xff, 0xff, 0xff  // SETTINGS_INITIAL_WINDOW_SIZE = 2147483647 (2GB)
    };
    if (h2_write_frame(conn, CWIST_HTTP2_FRAME_SETTINGS, 0, 0, settings, sizeof(settings)) != 0) {
        h2_conn_destroy(&hc);
        result = make_error(CWIST_ERR_INT16);
        result.error.err_i16 = -1;
        return result;
    }

    /* Upgrade connection flow control window to 2GB */
    h2_send_window_update(&hc, 0, 2147483647 - 65535);

    bool connected = true;
    bool sent_goaway = false;
    while (connected && atomic_load(&g_cwist_running)) {
        unsigned char hdr[9];
        int offset = 0;
        while (offset < 9) {
            int n = h2_read(conn, hdr + offset, 9 - offset);
            if (n <= 0) { connected = false; break; }
            offset += n;
        }
        if (!connected) break;

        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | hdr[2];
        uint8_t type = hdr[3];
        uint8_t flags = hdr[4];
        uint32_t stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) |
                             ((uint32_t)hdr[6] << 16) |
                             ((uint32_t)hdr[7] << 8) |
                             hdr[8];

        /* Frame size validation */
        if (len > CWIST_HTTP2_MAX_FRAME_SIZE) {
            if (type == CWIST_HTTP2_FRAME_SETTINGS || type == CWIST_HTTP2_FRAME_HEADERS ||
                type == CWIST_HTTP2_FRAME_CONTINUATION || type == CWIST_HTTP2_FRAME_PUSH_PROMISE) {
                if (type == CWIST_HTTP2_FRAME_SETTINGS || len > hc.peer_max_frame_size) {
                    h2_send_goaway(&hc, hc.last_processed_stream_id, H2_ERR_FRAME_SIZE_ERROR);
                    connected = false;
                    break;
                }
            }
        }

        unsigned char *payload = NULL;
        if (len > 0) {
            payload = (unsigned char *)cwist_alloc(len);
            if (!payload) { connected = false; break; }
            int off = 0;
            while (off < (int)len) {
                int r = h2_read(conn, payload + off, len - off);
                if (r <= 0) { connected = false; break; }
                off += r;
            }
        }
        if (!connected) { cwist_free(payload); break; }

        switch (type) {
            case CWIST_HTTP2_FRAME_SETTINGS: {
                if (h2_handle_settings(&hc, payload, len, flags & CWIST_HTTP2_FLAG_ACK) != 0) {
                    h2_send_goaway(&hc, hc.last_processed_stream_id, H2_ERR_PROTOCOL_ERROR);
                    connected = false;
                } else if ((flags & CWIST_HTTP2_FLAG_ACK) == 0) {
                    if (h2_write_frame(conn, CWIST_HTTP2_FRAME_SETTINGS, CWIST_HTTP2_FLAG_ACK, 0, NULL, 0) != 0) {
                        connected = false;
                    }
                }
                break;
            }

            case CWIST_HTTP2_FRAME_HEADERS: {
                if (stream_id == 0 || (stream_id % 2) == 0) {
                    h2_send_goaway(&hc, 0, H2_ERR_PROTOCOL_ERROR);
                    connected = false;
                    break;
                }
                /* Strip PADDED and PRIORITY from payload for decoding */
                size_t block_offset = 0;
                size_t block_len = len;
                if ((flags & CWIST_HTTP2_FLAG_PADDED) != 0 && block_len > 0) {
                    uint8_t pad_len = payload[block_offset++];
                    block_len--;
                    if (pad_len <= block_len) block_len -= pad_len;
                }
                if ((flags & CWIST_HTTP2_FLAG_PRIORITY) != 0 && block_len >= 5) {
                    block_offset += 5;
                    block_len -= 5;
                }
                if (h2_begin_headers(&hc, stream_id,
                                     payload + block_offset, block_len,
                                     flags & CWIST_HTTP2_FLAG_END_HEADERS,
                                     flags & CWIST_HTTP2_FLAG_END_STREAM) != 0) {
                    h2_send_goaway(&hc, hc.last_processed_stream_id, H2_ERR_PROTOCOL_ERROR);
                    connected = false;
                    break;
                }
                hc.last_processed_stream_id = stream_id;
                if (flags & CWIST_HTTP2_FLAG_END_STREAM) {
                    h2_stream *s = h2_stream_find(&hc, stream_id);
                    if (s && s->req) {
                        cwist_http_response *res = cwist_http_response_create();
                        if (res) {
                            handler(user_ctx, s->req, res);
                            h2_inject_alt_svc(conn, res);
                            if (h2_send_response_hc(&hc, stream_id, res) != 0) connected = false;
                            cwist_http_response_destroy(res);
                        }
                        h2_stream_remove(&hc, stream_id);
                    }
                }
                break;
            }

            case CWIST_HTTP2_FRAME_CONTINUATION: {
                if (stream_id == 0 || !hc.expecting_continuation || hc.cont_stream_id != stream_id) {
                    h2_send_goaway(&hc, hc.last_processed_stream_id, H2_ERR_PROTOCOL_ERROR);
                    connected = false;
                    break;
                }
                if (h2_handle_continuation(&hc, stream_id, payload, len,
                                           flags & CWIST_HTTP2_FLAG_END_HEADERS) != 0) {
                    h2_send_goaway(&hc, hc.last_processed_stream_id, H2_ERR_PROTOCOL_ERROR);
                    connected = false;
                    break;
                }
                if (flags & CWIST_HTTP2_FLAG_END_STREAM) {
                    /* END_STREAM on CONTINUATION is a protocol violation */
                    h2_send_goaway(&hc, hc.last_processed_stream_id, H2_ERR_PROTOCOL_ERROR);
                    connected = false;
                    break;
                }
                if ((flags & CWIST_HTTP2_FLAG_END_HEADERS) && hc.cont_end_stream) {
                    h2_stream *s = h2_stream_find(&hc, stream_id);
                    if (s && s->req) {
                        cwist_http_response *res = cwist_http_response_create();
                        if (res) {
                            handler(user_ctx, s->req, res);
                            h2_inject_alt_svc(conn, res);
                            if (h2_send_response_hc(&hc, stream_id, res) != 0) connected = false;
                            cwist_http_response_destroy(res);
                        }
                        h2_stream_remove(&hc, stream_id);
                    }
                }
                break;
            }

            case CWIST_HTTP2_FRAME_DATA: {
                if (stream_id == 0) {
                    h2_send_goaway(&hc, 0, H2_ERR_PROTOCOL_ERROR);
                    connected = false;
                    break;
                }
                h2_stream *s = h2_stream_find(&hc, stream_id);
                if (!s) {
                    /* DATA on closed/unknown stream: must check if it violates flow control */
                    /* For simplicity, we treat it as a stream error by ignoring the data
                     * but still accounting connection-level window. */
                    hc.conn_recv_window -= (int32_t)len;
                    if (hc.conn_recv_window < 0) {
                        h2_send_goaway(&hc, hc.last_processed_stream_id, H2_ERR_FLOW_CONTROL_ERROR);
                        connected = false;
                    }
                    break;
                }
                if (len > 0) {
                    hc.conn_recv_window -= (int32_t)len;
                    s->recv_window -= (int32_t)len;
                    if (hc.conn_recv_window < 0 || s->recv_window < 0) {
                        h2_send_goaway(&hc, hc.last_processed_stream_id, H2_ERR_FLOW_CONTROL_ERROR);
                        connected = false;
                        break;
                    }
                }
                if (payload && len > 0) {
                    if (s->req->body->size + len > CWIST_HTTP_MAX_BODY_SIZE) {
                        uint8_t rst[4] = {0, 0, 0, H2_ERR_REFUSED_STREAM};
                        h2_write_frame(hc.conn, CWIST_HTTP2_FRAME_RST_STREAM, 0, stream_id, rst, 4);
                        h2_stream_remove(&hc, stream_id);
                        break;
                    }
                    cwist_sstring_append_len(s->req->body, (const char *)payload, len);
                }
                if (flags & CWIST_HTTP2_FLAG_END_STREAM) {
                    cwist_http_response *res = cwist_http_response_create();
                    if (res) {
                        handler(user_ctx, s->req, res);
                        h2_inject_alt_svc(conn, res);
                        if (h2_send_response_hc(&hc, stream_id, res) != 0) connected = false;
                        cwist_http_response_destroy(res);
                    }
                    h2_stream_remove(&hc, stream_id);
                }
                if (connected) {
                    h2_auto_window_update(&hc, s);
                }
                break;
            }

            case CWIST_HTTP2_FRAME_RST_STREAM: {
                if (stream_id == 0 || len != 4) {
                    h2_send_goaway(&hc, hc.last_processed_stream_id, H2_ERR_PROTOCOL_ERROR);
                    connected = false;
                    break;
                }
                h2_stream_remove(&hc, stream_id);
                break;
            }

            case CWIST_HTTP2_FRAME_PING: {
                if (len != 8) {
                    h2_send_goaway(&hc, hc.last_processed_stream_id, H2_ERR_FRAME_SIZE_ERROR);
                    connected = false;
                    break;
                }
                if ((flags & CWIST_HTTP2_FLAG_ACK) == 0) {
                    if (h2_write_frame(conn, CWIST_HTTP2_FRAME_PING, CWIST_HTTP2_FLAG_ACK, 0, payload, 8) != 0) {
                        connected = false;
                    }
                }
                break;
            }

            case CWIST_HTTP2_FRAME_GOAWAY: {
                connected = false;
                break;
            }

            case CWIST_HTTP2_FRAME_WINDOW_UPDATE: {
                if (len != 4) {
                    h2_send_goaway(&hc, hc.last_processed_stream_id, H2_ERR_FRAME_SIZE_ERROR);
                    connected = false;
                    break;
                }
                if (h2_handle_window_update(&hc, stream_id, payload, len) != 0) {
                    h2_send_goaway(&hc, hc.last_processed_stream_id, H2_ERR_FLOW_CONTROL_ERROR);
                    connected = false;
                }
                break;
            }

            default:
                /* Unknown frames MUST be ignored (RFC 7540 Section 5.5) */
                break;
        }

        cwist_free(payload);
    }

    if (connected && !atomic_load(&g_cwist_running) && !sent_goaway) {
        h2_send_goaway(&hc, hc.last_processed_stream_id, H2_ERR_NO_ERROR);
        sent_goaway = true;
    }

    h2_conn_destroy(&hc);

    result = make_error(CWIST_ERR_INT16);
    result.error.err_i16 = 0;
    return result;
}

/* ------------------------------------------------------------------ */
/* HTTP/2 Server Push                                                 */
/* ------------------------------------------------------------------ */

int cwist_http2_push_resource(cwist_http_request *req,
                              const char *path,
                              const char *content_type,
                              const unsigned char *data,
                              size_t data_len) {
    if (!req || !req->private_data || !path) return -1;

    cwist_https_connection *conn = (cwist_https_connection *)req->private_data;
    uint32_t original_stream_id = req->stream_id;

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

    static _Atomic uint32_t next_server_stream = 2;
    uint32_t promised_stream_id = atomic_fetch_add(&next_server_stream, 2);

    unsigned char header_block[4096];
    size_t pos = 0;

    if (pos >= sizeof(header_block)) return -1;
    header_block[pos++] = 0x82;

    if (pos >= sizeof(header_block)) return -1;
    header_block[pos++] = 0x87;

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

    unsigned char pp_payload[4096 + 4];
    pp_payload[0] = (unsigned char)((promised_stream_id >> 24) & 0x7f);
    pp_payload[1] = (unsigned char)((promised_stream_id >> 16) & 0xff);
    pp_payload[2] = (unsigned char)((promised_stream_id >> 8) & 0xff);
    pp_payload[3] = (unsigned char)(promised_stream_id & 0xff);
    memcpy(pp_payload + 4, header_block, pos);

    if (h2_write_frame(conn, 0x05, CWIST_HTTP2_FLAG_END_HEADERS,
                       original_stream_id, pp_payload, (uint32_t)(pos + 4)) != 0) {
        return -1;
    }

    cwist_http_response *res = cwist_http_response_create();
    if (!res) return -1;
    res->status_code = CWIST_HTTP_OK;
    if (content_type) {
        cwist_http_header_add(&res->headers, "content-type", content_type);
    }
    if (data && data_len > 0) {
        cwist_sstring_assign_len(res->body, (const char *)data, data_len);
    }

    int ret = h2_send_response_raw(conn, promised_stream_id, res, CWIST_HTTP2_MAX_FRAME_SIZE);
    cwist_http_response_destroy(res);
    return ret;
}
