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
#include <strings.h>
#include <poll.h>
#include <fcntl.h>
#include <pthread.h>
#include <ctype.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>

#include <cwist/net/http/http2.h>
#include <cwist/net/http/http2_flow_control.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/log.h>
#include <cwist/sys/metrics/metrics.h>
#include <cwist/core/seq/seq.h>
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

/* CWIST sequenced DATA extension: each DATA frame carries one seq chunk.
 * Disabled by default so that standard HTTP/2 clients receive plain DATA
 * payloads.  Applications that want the CWIST-specific sequenced stream can
 * enable it per-connection via cwist_https_connection.http2_sequenced_data. */
#define CWIST_HTTP2_USE_SEQUENCED_DATA 0
#define CWIST_HTTP2_SEQ_CHUNK_PAYLOAD(max_frame) \
    ((uint16_t)((max_frame) > CWIST_SEQ_HEADER_SIZE ? (max_frame) - CWIST_SEQ_HEADER_SIZE : 0))

#define H2_ERR_NO_ERROR           0x0
#define H2_ERR_PROTOCOL_ERROR     0x1
#define H2_ERR_FLOW_CONTROL_ERROR 0x3
#define H2_ERR_SETTINGS_TIMEOUT   0x4
#define H2_ERR_STREAM_CLOSED      0x5
#define H2_ERR_FRAME_SIZE_ERROR   0x6
#define H2_ERR_REFUSED_STREAM     0x7
#define H2_ERR_COMPRESSION_ERROR  0x9

/* Monotonic clock in milliseconds, for the connection idle deadline. */
static uint64_t h2_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Monotonic clock in microseconds, for flow-control RTT samples/pacing. */
static uint64_t h2_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

/* Idle deadline for a connection with no complete frame arriving.  Defaults
 * to CWIST_HTTP2_IDLE_TIMEOUT_MS, overridable via the env var of the same
 * name (read once at first use). */
static int h2_idle_timeout_ms(void) {
    static int timeout_ms = 0;
    if (timeout_ms == 0) {
        const char *env = getenv("CWIST_HTTP2_IDLE_TIMEOUT_MS");
        timeout_ms = (env && atoi(env) > 0) ? atoi(env) : CWIST_HTTP2_IDLE_TIMEOUT_MS;
    }
    return timeout_ms;
}

/* --- Stream & Connection State --- */

typedef struct h2_stream {
    uint32_t stream_id;
    cwist_http_request *req;
    /* Adaptive flow control (send: peer-advertised credit; receive: our
     * advertised credit).  Replaces the old hand-rolled window ints. */
    cwist_http2_stream_flow_control fc;
    bool send_aborted;       /* RST_STREAM received while sending */
    uint8_t recv_xor;      /* running XOR of received DATA payload bytes */
    uint8_t send_xor;      /* running XOR of sent DATA payload bytes */
    cwist_seq_assembler_t *body_assembler; /* reorders sequenced DATA chunks */
    struct h2_stream *next;
} h2_stream;

typedef struct h2_deferred_frame {
    unsigned char hdr[9];
    unsigned char *payload; /* owned; NULL when the frame has no payload */
    struct h2_deferred_frame *next;
} h2_deferred_frame;

typedef struct {
    cwist_https_connection *conn;
    h2_stream *streams;
    uint32_t peer_max_frame_size;
    uint32_t peer_initial_window_size;
    uint32_t peer_max_concurrent_streams;
    /* Connection-level adaptive flow control: tracks both our advertised
     * receive credit and the peer-advertised send credit, auto-tunes the
     * receive target from PING-measured RTT, and paces sends once RTT is
     * known. */
    cwist_http2_flow_control fc;
    uint64_t ping_sent_us;
    unsigned char ping_payload[8];
    bool ping_outstanding;
    unsigned char *cont_buf;
    size_t cont_len;
    size_t cont_cap;
    uint32_t cont_stream_id;
    bool expecting_continuation;
    bool cont_end_stream;
    bool sequenced_data;   /* CWIST extension: DATA frames carry seq chunks */
    uint32_t last_processed_stream_id;
    uint64_t last_activity; /* monotonic ms of the last complete frame read */
    /* Frames the send-window wait loop had to read off the wire but must not
     * dispatch (HEADERS/SETTINGS of other streams, etc.). The main loop
     * drains these before touching the socket again. */
    h2_deferred_frame *deferred_head;
    h2_deferred_frame *deferred_tail;
} h2_conn;

static void h2_conn_init(h2_conn *hc, cwist_https_connection *conn) {
    memset(hc, 0, sizeof(*hc));
    hc->conn = conn;
    hc->peer_max_frame_size = CWIST_HTTP2_MAX_FRAME_SIZE;
    hc->peer_initial_window_size = 65535;
    /* Start at the RFC-default 65535 on the send side; the startup code
     * bumps our receive credit to 2GB on the wire and mirrors that into
     * fc.receive_window. */
    cwist_http2_flow_control_init(&hc->fc, 0, CWIST_HTTP2_MAX_WINDOW);
    hc->fc.send_window = 65535;
    hc->cont_end_stream = false;
    hc->last_activity = h2_now_ms();
    /* The extension carries application bodies and must not be enabled over
     * h2c: its ordering metadata is not an integrity mechanism.  HTTPS/TLS
     * supplies authenticated transport protection against on-path mutation. */
    hc->sequenced_data = conn && conn->ssl && conn->http2_sequenced_data;
}

static void h2_conn_destroy(h2_conn *hc) {
    h2_stream *s = hc->streams;
    while (s) {
        h2_stream *next = s->next;
        if (s->req) cwist_http_request_destroy(s->req);
        cwist_seq_assembler_destroy(s->body_assembler);
        cwist_free(s);
        s = next;
    }
    cwist_free(hc->cont_buf);
    h2_deferred_frame *df = hc->deferred_head;
    while (df) {
        h2_deferred_frame *next = df->next;
        cwist_free(df->payload);
        cwist_free(df);
        df = next;
    }
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
    memset(s, 0, sizeof(*s));
    s->stream_id = stream_id;
    /* Receive credit matches the INITIAL_WINDOW_SIZE=2GB we advertise in
     * SETTINGS; send credit starts at the peer's initial window. */
    cwist_http2_stream_flow_control_init(&s->fc, stream_id,
                                         CWIST_HTTP2_MAX_WINDOW, CWIST_HTTP2_MAX_WINDOW);
    s->fc.send_window = hc->peer_initial_window_size;
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
            cwist_seq_assembler_destroy(s->body_assembler);
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

/* Lightweight XOR checksum over a byte buffer. */
static uint8_t h2_xor_bytes(const unsigned char *buf, size_t len) {
    uint8_t x = 0;
    for (size_t i = 0; i < len; i++) x ^= buf[i];
    return x;
}

/* Poll the socket for the direction OpenSSL is waiting on, or for plain
 * socket writability. Returns 0 when ready, -1 on timeout/error. */
static int h2_wait_socket(cwist_https_connection *conn, int ssl_error, int timeout_ms) {
    struct pollfd pfd = { .fd = conn->fd, .events = 0 };
    if (conn->ssl) {
        if (ssl_error == SSL_ERROR_WANT_READ) {
            pfd.events = POLLIN;
        } else if (ssl_error == SSL_ERROR_WANT_WRITE) {
            pfd.events = POLLOUT;
        } else {
            return -1;
        }
    } else {
        pfd.events = POLLOUT;
    }
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return -1;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    return 0;
}

/* Poll until frame bytes can be read or the connection idle deadline
 * expires.  Skips the poll when decrypted bytes are already buffered.
 * Returns 0 when readable, -1 on idle timeout or socket error. */
static int h2_wait_readable(h2_conn *hc) {
    while (hc->conn->ssl ? (SSL_pending(hc->conn->ssl) <= 0) : true) {
        uint64_t idle_ms = (uint64_t)h2_idle_timeout_ms();
        uint64_t now = h2_now_ms();
        if (now - hc->last_activity >= idle_ms) return -1;
        uint64_t idle_left = idle_ms - (now - hc->last_activity);
        int wait_ms = CWIST_HTTP_TIMEOUT_MS;
        if (idle_left < (uint64_t)wait_ms) wait_ms = (int)idle_left;
        struct pollfd pfd = { .fd = hc->conn->fd, .events = POLLIN };
        int pret = poll(&pfd, 1, wait_ms);
        if (pret < 0) return -1;
        if (pret > 0) return 0;
    }
    return 0;
}

/* Read exactly len bytes, tolerating SSL_ERROR_WANT_READ/WANT_WRITE (and
 * plain EAGAIN) on the non-blocking sockets the pool hands us.  Bounded by
 * the connection idle deadline so a stalled peer cannot pin a worker.
 * Returns 0 on success, -1 on EOF, timeout, or socket error. */
static int h2_read_full(h2_conn *hc, void *buf, size_t len) {
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (got < len) {
        int n = h2_read(hc->conn, p + got, (int)(len - got));
        if (n > 0) {
            got += (size_t)n;
            hc->last_activity = h2_now_ms();
            continue;
        }
        if (n < 0) {
            int retryable = 0;
            int ssl_err = 0;
            if (hc->conn->ssl) {
                ssl_err = SSL_get_error(hc->conn->ssl, n);
                retryable = (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE);
            } else {
                retryable = (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
            }
            if (retryable) {
                uint64_t idle_ms = (uint64_t)h2_idle_timeout_ms();
                uint64_t now = h2_now_ms();
                if (now - hc->last_activity >= idle_ms) return -1;
                uint64_t left = idle_ms - (now - hc->last_activity);
                int wait_ms = CWIST_HTTP_TIMEOUT_MS;
                if (left < (uint64_t)wait_ms) wait_ms = (int)left;
                if (hc->conn->ssl) {
                    if (h2_wait_socket(hc->conn, ssl_err, wait_ms) != 0) return -1;
                } else {
                    struct pollfd pfd = { .fd = hc->conn->fd, .events = POLLIN };
                    if (poll(&pfd, 1, wait_ms) <= 0) return -1;
                }
                continue;
            }
        }
        return -1; /* EOF (0) or a real read error */
    }
    return 0;
}

static int h2_write_all(cwist_https_connection *conn, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    int retries = 0;
    /* 100ms polls; keep the total wait under the connection idle deadline. */
    int max_retries = h2_idle_timeout_ms() / 100;
    if (max_retries > 1000) max_retries = 1000;
    if (max_retries < 1) max_retries = 1;
    while (len > 0) {
        int n = h2_write(conn, p, (int)len);
        if (n <= 0) {
            if (n < 0) {
                if (conn->ssl) {
                    int err = SSL_get_error(conn->ssl, n);
                    if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                        if (++retries > max_retries) return -1; /* ~overall timeout guard */
                        if (h2_wait_socket(conn, err, 100) != 0) return -1;
                        continue;
                    }
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                        if (++retries > max_retries) return -1;
                        if (h2_wait_socket(conn, 0, 100) != 0) return -1;
                        continue;
                    }
                }
            }
            return -1;
        }
        retries = 0;
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

/* Adaptive receive-window refill driven by the flow-control module: emits a
 * WINDOW_UPDATE only once at least half of the (RTT-scaled) target window
 * has been consumed, instead of the old fixed 32 KiB threshold. */
static int h2_auto_window_update(h2_conn *hc, h2_stream *s) {
    unsigned char frame_buf[13];
    size_t written = 0;
    uint64_t now = h2_now_us();

    int r = cwist_http2_flow_control_maybe_window_update(&hc->fc, frame_buf,
                                                         sizeof(frame_buf), &written,
                                                         now, false);
    if (r < 0) return -1;
    if (r > 0 && h2_write_all(hc->conn, frame_buf, written) != 0) return -1;

    if (s) {
        written = 0;
        r = cwist_http2_stream_flow_control_maybe_window_update(&hc->fc, &s->fc,
                                                                frame_buf, sizeof(frame_buf),
                                                                &written, now, false);
        if (r < 0) return -1;
        if (r > 0 && h2_write_all(hc->conn, frame_buf, written) != 0) return -1;
    }
    return 0;
}

/* How many bytes may be sent right now: peer window credit, throttled by
 * RTT pacing once the first PING sample has calibrated it. */
static uint32_t h2_send_allowance(h2_conn *hc, h2_stream *s, uint32_t requested) {
    uint32_t allowed = requested;
    if (allowed > hc->fc.send_window) allowed = hc->fc.send_window;
    if (s && allowed > s->fc.send_window) allowed = s->fc.send_window;
    if (s && hc->fc.rtt_initialized && allowed > 0) {
        size_t paced = cwist_http2_flow_control_pacing_allowance(&hc->fc, &s->fc,
                                                                 allowed, h2_now_us());
        if (paced < allowed) allowed = (uint32_t)paced;
    }
    return allowed;
}

static void h2_commit_send(h2_conn *hc, h2_stream *s, uint32_t bytes) {
    if (s && hc->fc.rtt_initialized) {
        /* Allowance was already vetted by h2_send_allowance, so this always
         * succeeds and debits windows + pacing tokens together. */
        cwist_http2_flow_control_reserve_send(&hc->fc, &s->fc, bytes, h2_now_us());
        return;
    }
    hc->fc.send_window -= bytes;
    if (s) s->fc.send_window -= bytes;
}

/* Send a PING carrying a monotonic timestamp; the matching ACK yields an
 * RTT sample that feeds the adaptive window/pacing tuning. */
static int h2_send_ping(h2_conn *hc) {
    uint64_t now = h2_now_us();
    unsigned char p[8];
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(now >> (56 - 8 * i));
    if (h2_write_frame(hc->conn, CWIST_HTTP2_FRAME_PING, 0, 0, p, 8) != 0) return -1;
    memcpy(hc->ping_payload, p, 8);
    hc->ping_sent_us = now;
    hc->ping_outstanding = true;
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

typedef struct {
    int16_t symbol;
    uint8_t bits;
} h2_huffman_lut_entry;

static h2_huffman_lut_entry h2_huffman_root_lut[256];

static h2_huffman_node h2_huffman_pool[4096];
static size_t h2_huffman_pool_used = 0;
static h2_huffman_node *h2_huffman_root = NULL;
/* The pool is shared mutable state; build the trie exactly once instead of
 * letting concurrent first connections race through lazy init (a lost race
 * could hand a half-built trie to a decoder or exhaust the pool twice). */
static pthread_once_t h2_huffman_once = PTHREAD_ONCE_INIT;

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

    h2_huffman_node *root = h2_huffman_alloc_node();
    if (!root) return;
    for (int sym = 0; sym <= 256; ++sym) {
        uint32_t code = table[sym].code;
        uint8_t bits = table[sym].bits;
        h2_huffman_node *node = root;
        for (int i = bits - 1; i >= 0; --i) {
            int bit = (int)((code >> i) & 1);
            if (!node->child[bit]) node->child[bit] = h2_huffman_alloc_node();
            if (!node->child[bit]) return; /* pool exhausted: leave root unset */
            node = node->child[bit];
        }
        node->symbol = sym;
        node->is_terminal = true;
    }
    h2_huffman_root = root;
    
    /* Precompute the 8-bit lookup table for root node transitions. */
    for (int i = 0; i < 256; ++i) {
        h2_huffman_node *n = h2_huffman_root;
        int16_t decoded_sym = -1;
        uint8_t consumed_bits = 0;
        for (int b = 7; b >= 0; --b) {
            int bit = (i >> b) & 1;
            n = n->child[bit];
            if (!n) {
                break;
            }
            if (n->is_terminal) {
                decoded_sym = (int16_t)n->symbol;
                consumed_bits = (uint8_t)(8 - b);
                break;
            }
        }
        h2_huffman_root_lut[i].symbol = decoded_sym;
        h2_huffman_root_lut[i].bits = consumed_bits;
    }
}

char *h2_huffman_decode(const unsigned char *src, size_t src_len, size_t *out_len) {
    pthread_once(&h2_huffman_once, h2_huffman_init);
    if (!h2_huffman_root) return NULL;
    size_t cap = src_len * 2 + 1;
    if (cap < 16) cap = 16;
    char *out = (char *)cwist_alloc(cap);
    if (!out) return NULL;
    size_t out_pos = 0;

    uint32_t bit_buf = 0;
    int bit_cnt = 0;
    size_t src_idx = 0;
    h2_huffman_node *node = h2_huffman_root;

    while (src_idx < src_len || bit_cnt > 0) {
        /* Keep the bit buffer filled with up to 24 bits. */
        while (bit_cnt <= 24 && src_idx < src_len) {
            bit_buf = (bit_buf << 8) | src[src_idx++];
            bit_cnt += 8;
        }

        /* Speed-up: If we are at the root, try decoding 8 bits using the LUT. */
        if (node == h2_huffman_root && bit_cnt >= 8) {
            uint8_t index = (uint8_t)(bit_buf >> (bit_cnt - 8));
            h2_huffman_lut_entry entry = h2_huffman_root_lut[index];
            if (entry.symbol != -1) {
                if (entry.symbol == 256) {
                    cwist_free(out);
                    return NULL;
                }
                if (out_pos + 1 >= cap) {
                    cap *= 2;
                    char *tmp = (char *)cwist_realloc(out, cap);
                    if (!tmp) { cwist_free(out); return NULL; }
                    out = tmp;
                }
                out[out_pos++] = (char)entry.symbol;
                bit_cnt -= entry.bits;
                continue;
            }
        }

        /* Fallback: bit-by-bit traversal. */
        if (bit_cnt > 0) {
            int bit = (bit_buf >> (bit_cnt - 1)) & 1;
            bit_cnt--;
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
        } else {
            break;
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
            req->query_params = cwist_query_map_create_in_arena(req->arena);
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
            if (entry && entry->name) {
                h2_apply_header(req, entry->name, entry->value);
            } else {
                /* Client referenced the HPACK dynamic table despite
                 * SETTINGS_HEADER_TABLE_SIZE=0: the header is lost, so make
                 * the loss observable instead of a silent session break. */
                cwist_metric_inc(cwist_metrics_registry(), CWIST_METRIC_H2_HEADERS_DROPPED);
                CWIST_LOG_WARN("[h2] dropping header field: indexed name/value index=%u beyond static table", index);
            }
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
                /* Same dynamic-table reference case as above, on the name
                 * side of a literal field. */
                cwist_metric_inc(cwist_metrics_registry(), CWIST_METRIC_H2_HEADERS_DROPPED);
                CWIST_LOG_WARN("[h2] dropping header field: literal with name index=%u beyond static table", name_index);
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

/* RFC 9113 §8.1.1: responses with 1xx/204/304 status carry no content, and
 * content-length is forbidden on 1xx/204 (and meaningless on 304 here). */
static bool h2_status_forbids_body(int status_code) {
    return (status_code >= 100 && status_code < 200) ||
           status_code == 204 || status_code == 304;
}

static size_t h2_encode_response_headers(cwist_http_response *res,
                                          unsigned char *dst, size_t dst_cap) {
    size_t pos = 0;
    const bool bodyless = h2_status_forbids_body(res->status_code);

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

    if (!bodyless && !headers_have_content_length(res->headers)) {
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
        /* RFC 9113 §8.2.2: connection-specific fields are malformed in
         * HTTP/2; te is only allowed with the value "trailers". */
        if (strcasecmp(curr->key->data, "connection") == 0 ||
            strcasecmp(curr->key->data, "keep-alive") == 0 ||
            strcasecmp(curr->key->data, "proxy-connection") == 0 ||
            strcasecmp(curr->key->data, "transfer-encoding") == 0 ||
            strcasecmp(curr->key->data, "upgrade") == 0 ||
            (strcasecmp(curr->key->data, "te") == 0 &&
             strcasecmp(curr->value->data, "trailers") != 0)) {
            curr = curr->next;
            continue;
        }
        if (bodyless && strcasecmp(curr->key->data, "content-length") == 0) {
            curr = curr->next;
            continue;
        }
        /* NUL/CR/LF in a field value makes the response malformed; drop the
         * offending field instead of poisoning the whole response. */
        if (strpbrk(curr->value->data, "\r\n") ||
            strlen(curr->value->data) != curr->value->size) {
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

/* Send an encoded header block as HEADERS, splitting into CONTINUATION
 * frames when it exceeds the peer's advertised max frame size.  base_flags
 * (e.g. END_STREAM) applies to the HEADERS frame only. */
static int h2_send_header_block(cwist_https_connection *conn, uint32_t stream_id,
                                const unsigned char *block, size_t block_len,
                                uint32_t max_frame, uint8_t base_flags) {
    size_t sent = 0;
    uint8_t type = CWIST_HTTP2_FRAME_HEADERS;
    uint8_t flags = base_flags;
    while (true) {
        size_t chunk = block_len - sent;
        if (chunk > max_frame) chunk = max_frame;
        uint8_t frame_flags = flags;
        if (sent + chunk == block_len) frame_flags |= CWIST_HTTP2_FLAG_END_HEADERS;
        if (h2_write_frame(conn, type, frame_flags, stream_id,
                           block + sent, (uint32_t)chunk) != 0) {
            return -1;
        }
        sent += chunk;
        if (sent == block_len) return 0;
        type = CWIST_HTTP2_FRAME_CONTINUATION;
        flags = 0; /* END_STREAM is illegal on CONTINUATION */
    }
}

/* Encode response headers into a heap buffer that doubles until the block
 * fits (up to 1 MiB).  The old fixed 8 KiB stack buffer made any response
 * with a large header set (big cookies, JWTs) abort the whole connection,
 * which browsers surface as ERR_INVALID_RESPONSE. */
static unsigned char *h2_encode_response_block(cwist_http_response *res, size_t *out_len) {
    size_t cap = 16384;
    unsigned char *buf = NULL;
    while (cap <= (1u << 20)) {
        unsigned char *nb = (unsigned char *)cwist_realloc(buf, cap);
        if (!nb) {
            cwist_free(buf);
            return NULL;
        }
        buf = nb;
        size_t n = h2_encode_response_headers(res, buf, cap);
        if (n > 0) {
            *out_len = n;
            return buf;
        }
        cap *= 2;
    }
    cwist_free(buf);
    return NULL;
}

/**
 * @brief Send a memory body as sequenced DATA frames.
 *
 * Used by the CWIST HTTP/2 DATA extension: every DATA frame carries one
 * sequence chunk so the receiver can reorder and discard duplicates.
 */
static int h2_send_seq_data_frames(cwist_https_connection *conn,
                                   uint32_t stream_id,
                                   const uint8_t *body_data,
                                   size_t body_len,
                                   uint32_t max_frame_size,
                                   uint8_t *send_xor) {
    uint16_t chunk_payload = CWIST_HTTP2_SEQ_CHUNK_PAYLOAD(max_frame_size);
    if (chunk_payload == 0) return -1;

    cwist_seq_message_t msg;
    if (!cwist_seq_split(body_data, body_len, chunk_payload, &msg)) return -1;

    for (size_t i = 0; i < msg.count; i++) {
        uint8_t flags = (i + 1 == msg.count) ? CWIST_HTTP2_FLAG_END_STREAM : 0;
        if (h2_write_frame(conn, CWIST_HTTP2_FRAME_DATA, flags, stream_id,
                           msg.chunks[i], (uint32_t)msg.chunk_lens[i]) != 0) {
            cwist_seq_message_free(&msg);
            return -1;
        }
        if (send_xor) *send_xor ^= h2_xor_bytes(msg.chunks[i], msg.chunk_lens[i]);
    }

    cwist_seq_message_free(&msg);
    return 0;
}

static int h2_send_response_raw(cwist_https_connection *conn, uint32_t stream_id,
                                 cwist_http_response *res, uint32_t max_frame_size) {
    size_t block_len = 0;
    unsigned char *block = h2_encode_response_block(res, &block_len);
    if (!block) return -1;

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

    uint8_t header_flags = 0;
    if (body_len == 0 || h2_status_forbids_body(res->status_code)) {
        body_len = 0;
        header_flags |= CWIST_HTTP2_FLAG_END_STREAM;
    }

    int rc = h2_send_header_block(conn, stream_id, block, block_len, max_frame_size, header_flags);
    cwist_free(block);
    if (rc != 0) return -1;

    if (body_len == 0) return 0;

    if (res->use_file_stream && res->file_stream_fd >= 0) {
        /* Load file contents and sequence them.  Server push rarely streams
         * huge files, so a single read is acceptable here. */
        unsigned char *file_buf = (unsigned char *)cwist_alloc(body_len);
        if (!file_buf) return -1;
        ssize_t r = pread(res->file_stream_fd, file_buf, body_len, res->file_stream_offset);
        if (r <= 0 || (size_t)r != body_len) { cwist_free(file_buf); return -1; }
        int rc = h2_send_seq_data_frames(conn, stream_id, file_buf, body_len, max_frame_size, NULL);
        cwist_free(file_buf);
        return rc;
    }

    return h2_send_seq_data_frames(conn, stream_id, body_data, body_len, max_frame_size, NULL);
}

static int h2_read_all(h2_conn *hc, void *buf, int len) {
    /* h2_read_full tolerates WANT_READ on the non-blocking TLS sockets;
     * 0/EOF and real errors stay fatal. */
    if (h2_read_full(hc, buf, (size_t)len) != 0) return -1;
    return len;
}

/* Returns 0 normally, -1 when the peer vanished mid-frame (EOF/error).
 *
 * Two traps are handled here:
 * 1. poll(fd, 0) alone misses WINDOW_UPDATEs sitting in OpenSSL's internal
 *    buffer (a whole TLS record is decrypted per SSL_read, so leftover
 *    frames have zero bytes pending on the socket). Without the
 *    SSL_pending check the window wait loop never sees the update and the
 *    connection is torn down mid-body — the "some chunks arrive, then
 *    stuck" symptom.
 * 2. Frames this loop is not responsible for (HEADERS/SETTINGS of other
 *    streams, etc.) used to be read and discarded. They are now queued so
 *    the main frame loop can dispatch them. */
static int h2_process_incoming_frames_nonblocking(h2_conn *hc, h2_stream *s) {
    struct pollfd pfd;
    pfd.fd = hc->conn->fd;
    pfd.events = POLLIN;

    while ((poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) ||
           (hc->conn->ssl && SSL_pending(hc->conn->ssl) > 0)) {
        unsigned char hdr[9];
        int n = h2_read_all(hc, hdr, 9);
        if (n != 9) return -1;

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
            if (!payload) return -1;
            int r = h2_read_all(hc, payload, len);
            if (r != (int)len) {
                cwist_free(payload);
                return -1;
            }
        }

        hc->last_activity = h2_now_ms();

        if (type == 0x08) { // WINDOW_UPDATE
            if (len == 4) {
                int32_t increment = (((int32_t)payload[0] & 0x7f) << 24) |
                                    ((int32_t)payload[1] << 16) |
                                    ((int32_t)payload[2] << 8) |
                                    (int32_t)payload[3];
                if (increment > 0) {
                    if (stream_id == 0) {
                        cwist_http2_flow_control_add_send_window(&hc->fc, (uint32_t)increment);
                    } else if (s && stream_id == s->stream_id) {
                        cwist_http2_stream_flow_control_add_send_window(&s->fc, (uint32_t)increment);
                    } else {
                        h2_stream *other = h2_stream_find(hc, stream_id);
                        if (other) cwist_http2_stream_flow_control_add_send_window(&other->fc, (uint32_t)increment);
                    }
                }
            }
        } else if (type == 0x06) { // PING
            if ((flags & 0x01) == 0 && len == 8) { // ACK flag is 0x01
                h2_write_frame(hc->conn, 0x06, 0x01, 0, payload, 8);
            }
        } else if (type == 0x03) { // RST_STREAM
            if (s && stream_id == s->stream_id) {
                s->send_aborted = true;
            }
        } else {
            /* Not ours to consume: hand the intact frame to the main loop. */
            h2_deferred_frame *df = (h2_deferred_frame *)cwist_alloc(sizeof(*df));
            if (!df) {
                cwist_free(payload);
                return -1;
            }
            memcpy(df->hdr, hdr, 9);
            df->payload = payload;
            df->next = NULL;
            if (hc->deferred_tail) hc->deferred_tail->next = df;
            else hc->deferred_head = df;
            hc->deferred_tail = df;
            continue;
        }
        cwist_free(payload);
    }
    return 0;
}

/**
 * @brief Compute a sequenced chunk payload size that fits the current window.
 */
static uint16_t h2_seq_chunk_payload(h2_conn *hc, h2_stream *s, uint32_t max_frame_size) {
    uint16_t max_payload = CWIST_HTTP2_SEQ_CHUNK_PAYLOAD(max_frame_size);
    uint32_t eff_window = h2_send_allowance(hc, s, max_payload + CWIST_SEQ_HEADER_SIZE);
    if (eff_window <= CWIST_SEQ_HEADER_SIZE) return 0;
    uint32_t allowed_payload = eff_window - CWIST_SEQ_HEADER_SIZE;
    return (allowed_payload < (uint32_t)max_payload) ? (uint16_t)allowed_payload : max_payload;
}

/**
 * @brief Send a file body as sequenced DATA frames with flow-control waits.
 */
static int h2_send_seq_file_body(h2_conn *hc, h2_stream *s, uint32_t stream_id,
                                 int fd, off_t offset, size_t body_len,
                                 uint32_t max_frame_size) {
    uint16_t chunk_payload = h2_seq_chunk_payload(hc, s, max_frame_size);
    if (chunk_payload == 0 || body_len == 0) return -1;

    uint32_t total_chunks = (uint32_t)((body_len + chunk_payload - 1) / chunk_payload);
    size_t remaining = body_len;
    off_t cur_offset = offset;

    for (uint32_t i = 0; i < total_chunks && remaining > 0; i++) {
        while (hc->fc.send_window == 0 || (s && s->fc.send_window == 0)) {
            if (h2_process_incoming_frames_nonblocking(hc, s) != 0) return -1;
            if (s && s->send_aborted) return -1;
            if (hc->fc.send_window == 0 || (s && s->fc.send_window == 0)) {
                /* Give up if no frame arrived within the idle deadline. */
                if (h2_now_ms() - hc->last_activity >= (uint64_t)h2_idle_timeout_ms()) return -1;
                struct timespec ts = {0, 2000000};
                nanosleep(&ts, NULL);
            }
        }

        size_t plen = remaining > chunk_payload ? chunk_payload : remaining;
        size_t chunk_len = CWIST_SEQ_HEADER_SIZE + plen;
        if (h2_send_allowance(hc, s, (uint32_t)chunk_len) < chunk_len) {
            /* Window too small for a full sequenced chunk; wait for update. */
            if (h2_process_incoming_frames_nonblocking(hc, s) != 0) return -1;
            if (h2_now_ms() - hc->last_activity >= (uint64_t)h2_idle_timeout_ms()) return -1;
            struct timespec ts = {0, 2000000};
            nanosleep(&ts, NULL);
            i--;
            continue;
        }

        unsigned char *chunk = (unsigned char *)cwist_alloc(chunk_len);
        if (!chunk) return -1;
        ssize_t r = pread(fd, chunk + CWIST_SEQ_HEADER_SIZE, plen, cur_offset);
        if (r <= 0 || (size_t)r != plen) { cwist_free(chunk); return -1; }
        cwist_seq_chunk_build_header(chunk, i + 1, (uint16_t)total_chunks,
                                     (uint16_t)plen, chunk_payload);
        uint8_t flags = (i + 1 == total_chunks) ? CWIST_HTTP2_FLAG_END_STREAM : 0;
        if (h2_write_frame(hc->conn, CWIST_HTTP2_FRAME_DATA, flags, stream_id,
                           chunk, (uint32_t)chunk_len) != 0) {
            cwist_free(chunk); return -1;
        }
        if (s) s->send_xor ^= h2_xor_bytes(chunk, chunk_len);
        cwist_free(chunk);
        h2_commit_send(hc, s, (uint32_t)chunk_len);
        cur_offset += (off_t)plen;
        remaining -= plen;
    }
    return 0;
}

/**
 * @brief Send a memory body as sequenced DATA frames with flow-control waits.
 */
static int h2_send_seq_memory_body(h2_conn *hc, h2_stream *s, uint32_t stream_id,
                                   const uint8_t *body_data, size_t body_len,
                                   uint32_t max_frame_size) {
    uint16_t chunk_payload = h2_seq_chunk_payload(hc, s, max_frame_size);
    if (chunk_payload == 0) return -1;

    cwist_seq_message_t msg;
    if (!cwist_seq_split(body_data, body_len, chunk_payload, &msg)) return -1;

    for (size_t i = 0; i < msg.count; i++) {
        while (hc->fc.send_window == 0 || (s && s->fc.send_window == 0)) {
            if (h2_process_incoming_frames_nonblocking(hc, s) != 0) return -1;
            if (s && s->send_aborted) return -1;
            if (hc->fc.send_window == 0 || (s && s->fc.send_window == 0)) {
                /* Give up if no frame arrived within the idle deadline. */
                if (h2_now_ms() - hc->last_activity >= (uint64_t)h2_idle_timeout_ms()) return -1;
                struct timespec ts = {0, 2000000};
                nanosleep(&ts, NULL);
            }
        }

        size_t chunk_len = msg.chunk_lens[i];
        if (h2_send_allowance(hc, s, (uint32_t)chunk_len) < chunk_len) {
            /* Window too small for a full sequenced chunk; wait for update. */
            if (h2_process_incoming_frames_nonblocking(hc, s) != 0) return -1;
            if (h2_now_ms() - hc->last_activity >= (uint64_t)h2_idle_timeout_ms()) return -1;
            struct timespec ts = {0, 2000000};
            nanosleep(&ts, NULL);
            i--;
            continue;
        }

        uint8_t flags = (i + 1 == msg.count) ? CWIST_HTTP2_FLAG_END_STREAM : 0;
        if (h2_write_frame(hc->conn, CWIST_HTTP2_FRAME_DATA, flags, stream_id,
                           msg.chunks[i], (uint32_t)chunk_len) != 0) {
            cwist_seq_message_free(&msg);
            return -1;
        }
        if (s) s->send_xor ^= h2_xor_bytes(msg.chunks[i], chunk_len);
        h2_commit_send(hc, s, (uint32_t)chunk_len);
    }

    cwist_seq_message_free(&msg);
    return 0;
}

static int h2_send_response_hc(h2_conn *hc, uint32_t stream_id, cwist_http_response *res) {
    h2_stream *s = h2_stream_find(hc, stream_id);

    size_t block_len = 0;
    unsigned char *block = h2_encode_response_block(res, &block_len);
    if (!block) return -1;

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

    /* HEAD responses and 1xx/204/304 statuses carry no content.  The
     * content-length header (already encoded from the real body length for
     * HEAD; suppressed by the encoder for bodyless statuses) stays, but no
     * DATA frames may follow. */
    bool no_content = h2_status_forbids_body(res->status_code) ||
                      (s && s->req && s->req->method == CWIST_HTTP_HEAD);

    uint8_t header_flags = 0;
    if (body_len == 0 || no_content) {
        body_len = 0;
        header_flags |= CWIST_HTTP2_FLAG_END_STREAM;
    }

    uint32_t max_frame = hc->peer_max_frame_size;
    if (max_frame == 0) max_frame = CWIST_HTTP2_MAX_FRAME_SIZE;

    int rc = h2_send_header_block(hc->conn, stream_id, block, block_len, max_frame, header_flags);
    cwist_free(block);
    if (rc != 0) return -1;

    if (body_len == 0) return 0;

    if (hc->sequenced_data) {
        if (res->use_file_stream && res->file_stream_fd >= 0) {
            return h2_send_seq_file_body(hc, s, stream_id, res->file_stream_fd,
                                         res->file_stream_offset, body_len, max_frame);
        }
        return h2_send_seq_memory_body(hc, s, stream_id, body_data, body_len, max_frame);
    }

    /* Fallback: standard HTTP/2 DATA frames without sequence headers. */
    if (res->use_file_stream && res->file_stream_fd >= 0) {
        off_t offset = res->file_stream_offset;
        size_t remaining = res->file_stream_len;
        while (remaining > 0) {
            while (hc->fc.send_window == 0 || (s && s->fc.send_window == 0)) {
                if (h2_process_incoming_frames_nonblocking(hc, s) != 0) return -1;
                if (s && s->send_aborted) return -1;
                if (hc->fc.send_window == 0 || (s && s->fc.send_window == 0)) {
                    /* Give up if no frame arrived within the idle deadline. */
                    if (h2_now_ms() - hc->last_activity >= (uint64_t)h2_idle_timeout_ms()) return -1;
                    struct timespec ts = {0, 2000000};
                    nanosleep(&ts, NULL);
                }
            }

            uint32_t chunk = (uint32_t)(remaining > max_frame ? max_frame : remaining);
            uint32_t allowed = h2_send_allowance(hc, s, chunk);
            if (allowed == 0) {
                /* Pacing throttled us below one byte; wait briefly. */
                struct timespec ts = {0, 2000000};
                nanosleep(&ts, NULL);
                continue;
            }

            unsigned char *chunk_buf = (unsigned char *)cwist_alloc(allowed);
            if (!chunk_buf) return -1;
            ssize_t r = pread(res->file_stream_fd, chunk_buf, allowed, offset);
            if (r <= 0) { cwist_free(chunk_buf); return -1; }
            uint8_t flags = (remaining == (size_t)r) ? CWIST_HTTP2_FLAG_END_STREAM : 0;
            if (h2_write_frame(hc->conn, CWIST_HTTP2_FRAME_DATA, flags, stream_id, chunk_buf, (uint32_t)r) != 0) {
                cwist_free(chunk_buf); return -1;
            }
            if (s) s->send_xor ^= h2_xor_bytes(chunk_buf, (size_t)r);
            cwist_free(chunk_buf);
            h2_commit_send(hc, s, (uint32_t)r);
            offset += r;
            remaining -= (size_t)r;
        }
    } else {
        size_t sent = 0;
        while (sent < body_len) {
            while (hc->fc.send_window == 0 || (s && s->fc.send_window == 0)) {
                if (h2_process_incoming_frames_nonblocking(hc, s) != 0) return -1;
                if (s && s->send_aborted) return -1;
                if (hc->fc.send_window == 0 || (s && s->fc.send_window == 0)) {
                    /* Give up if no frame arrived within the idle deadline. */
                    if (h2_now_ms() - hc->last_activity >= (uint64_t)h2_idle_timeout_ms()) return -1;
                    struct timespec ts = {0, 2000000};
                    nanosleep(&ts, NULL);
                }
            }

            size_t remaining = body_len - sent;
            uint32_t chunk = (uint32_t)(remaining > max_frame ? max_frame : remaining);
            uint32_t allowed = h2_send_allowance(hc, s, chunk);
            if (allowed == 0) {
                /* Pacing throttled us below one byte; wait briefly. */
                struct timespec ts = {0, 2000000};
                nanosleep(&ts, NULL);
                continue;
            }

            uint8_t flags = (sent + allowed == body_len) ? CWIST_HTTP2_FLAG_END_STREAM : 0;
            if (h2_write_frame(hc->conn, CWIST_HTTP2_FRAME_DATA, flags, stream_id,
                               body_data + sent, allowed) != 0) {
                return -1;
            }
            if (s) s->send_xor ^= h2_xor_bytes(body_data + sent, allowed);
            h2_commit_send(hc, s, allowed);
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
                        if (delta >= 0) {
                            uint32_t inc = (uint32_t)delta;
                            s->fc.send_window = inc > CWIST_HTTP2_MAX_WINDOW - s->fc.send_window
                                                    ? CWIST_HTTP2_MAX_WINDOW
                                                    : s->fc.send_window + inc;
                        } else {
                            uint32_t dec = (uint32_t)(-delta);
                            /* RFC 7540 allows the adjusted window to go
                             * negative; clamping to 0 is the safe subset. */
                            s->fc.send_window = dec > s->fc.send_window ? 0 : s->fc.send_window - dec;
                        }
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
        if ((uint64_t)hc->fc.send_window + (uint32_t)increment > CWIST_HTTP2_MAX_WINDOW) return -1;
        hc->fc.send_window += (uint32_t)increment;
    } else {
        h2_stream *s = h2_stream_find(hc, stream_id);
        if (!s) {
            /* RFC 7540: WINDOW_UPDATE on closed stream is a STREAM_CLOSED error
             * or may be ignored depending on state. We ignore for leniency. */
            return 0;
        }
        if ((uint64_t)s->fc.send_window + (uint32_t)increment > CWIST_HTTP2_MAX_WINDOW) return -1;
        s->fc.send_window += (uint32_t)increment;
    }
    return 0;
}

/* --- Preface Verification --- */

static int cwist_http2_verify_preface(h2_conn *hc) {
    char buffer[CWIST_HTTP2_CONNECTION_PREFACE_LEN];
    /* The preface can arrive in a later TLS record than the handshake, so a
     * non-blocking SSL_read may legitimately report WANT_READ here; wait it
     * out instead of dropping the connection. */
    if (h2_read_full(hc, buffer, CWIST_HTTP2_CONNECTION_PREFACE_LEN) != 0) {
        return -1;
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

    h2_conn hc;
    h2_conn_init(&hc, conn);

    if (cwist_http2_verify_preface(&hc) != 0) {
        h2_conn_destroy(&hc);
        result = make_error(CWIST_ERR_INT16);
        result.error.err_i16 = -1;
        return result;
    }

    unsigned char settings[12] = {
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, // SETTINGS_HEADER_TABLE_SIZE = 0 (Disable dynamic table compression to guarantee full literal header transmission)
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
    hc.fc.receive_window = CWIST_HTTP2_MAX_WINDOW;

    /* Kick off RTT sampling for the adaptive flow control; the ACK arrives
     * in the main loop below.  Failure is non-fatal: pacing simply stays
     * uncalibrated. */
    h2_send_ping(&hc);

    bool connected = true;
    bool sent_goaway = false;
    while (connected && atomic_load(&g_cwist_running)) {
        unsigned char hdr[9];
        unsigned char *payload = NULL;

        /* Frames the send-window wait loop pulled off the wire but could not
         * dispatch are served first; only then touch the socket. */
        h2_deferred_frame *df = hc.deferred_head;
        if (df) {
            hc.deferred_head = df->next;
            if (!hc.deferred_head) hc.deferred_tail = NULL;
            memcpy(hdr, df->hdr, 9);
            payload = df->payload;
            cwist_free(df);
        } else {
            /* Bound the wait for the next frame with the idle deadline so an
             * idle connection cannot monopolize a pool worker forever. */
            if (h2_wait_readable(&hc) != 0) {
                if (!sent_goaway &&
                    h2_now_ms() - hc.last_activity >= (uint64_t)h2_idle_timeout_ms()) {
                    h2_send_goaway(&hc, hc.last_processed_stream_id, H2_ERR_NO_ERROR);
                    sent_goaway = true;
                }
                connected = false;
                break;
            }
            /* Frame bytes already in flight may still straddle TLS records or
             * TCP segments; h2_read_full waits out WANT_READ instead of
             * treating it as a dropped connection. */
            if (h2_read_full(&hc, hdr, 9) != 0) { connected = false; break; }
        }

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
                    cwist_free(payload);
                    connected = false;
                    break;
                }
            }
        }

        if (!df && len > 0) {
            payload = (unsigned char *)cwist_alloc(len);
            if (!payload) { connected = false; break; }
            if (h2_read_full(&hc, payload, len) != 0) connected = false;
            if (!connected) { cwist_free(payload); break; }
        }

        hc.last_activity = h2_now_ms();

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
                    if (!cwist_http2_flow_control_receive(&hc.fc, len)) {
                        h2_send_goaway(&hc, hc.last_processed_stream_id, H2_ERR_FLOW_CONTROL_ERROR);
                        connected = false;
                    } else {
                        cwist_http2_flow_control_consume(&hc.fc, len);
                    }
                    break;
                }
                if (len > 0) {
                    if (!cwist_http2_flow_control_receive(&hc.fc, len) ||
                        !cwist_http2_stream_flow_control_receive(&s->fc, len)) {
                        h2_send_goaway(&hc, hc.last_processed_stream_id, H2_ERR_FLOW_CONTROL_ERROR);
                        connected = false;
                        break;
                    }
                    cwist_http2_flow_control_consume(&hc.fc, len);
                    cwist_http2_stream_flow_control_consume(&s->fc, len);
                }
                if (payload && len > 0) {
                    if (hc.sequenced_data && len >= CWIST_SEQ_HEADER_SIZE) {
                        cwist_seq_chunk_t chunk;
                        if (cwist_seq_chunk_parse(payload, len, &chunk)) {
                            if (!s->body_assembler) {
                                s->body_assembler = cwist_seq_assembler_create_limited(
                                    CWIST_HTTP_MAX_BODY_SIZE);
                                if (!s->body_assembler) {
                                    connected = false;
                                    break;
                                }
                            }
                            if (!cwist_seq_assembler_feed(s->body_assembler, &chunk)) {
                                /* A conflicting duplicate is corruption, not a
                                 * retransmission.  Refuse the stream without
                                 * exposing a mixed body to the application. */
                                uint8_t rst[4] = {0, 0, 0, H2_ERR_PROTOCOL_ERROR};
                                h2_write_frame(hc.conn, CWIST_HTTP2_FRAME_RST_STREAM,
                                               0, stream_id, rst, sizeof(rst));
                                h2_stream_remove(&hc, stream_id);
                                break;
                            }
                        }
                        /* Malformed sequence chunks are ignored (discarded). */
                    } else if (!hc.sequenced_data) {
                        if (s->req->body->size + len > CWIST_HTTP_MAX_BODY_SIZE) {
                            uint8_t rst[4] = {0, 0, 0, H2_ERR_REFUSED_STREAM};
                            h2_write_frame(hc.conn, CWIST_HTTP2_FRAME_RST_STREAM, 0, stream_id, rst, 4);
                            h2_stream_remove(&hc, stream_id);
                            break;
                        }
                        s->recv_xor ^= h2_xor_bytes(payload, len);
                        cwist_sstring_append_len(s->req->body, (const char *)payload, len);
                    }
                }
                if (flags & CWIST_HTTP2_FLAG_END_STREAM) {
                    /* No assembler means no sequenced chunks ever arrived;
                     * an empty DATA END_STREAM is a complete empty body. */
                    bool sequence_complete = !hc.sequenced_data || !s->body_assembler;
                    if (hc.sequenced_data && s->body_assembler) {
                        const uint8_t *assembled = NULL;
                        size_t assembled_len = 0;
                        if (cwist_seq_assembler_get_data(s->body_assembler, &assembled, &assembled_len)) {
                            if (s->req->body->size + assembled_len <= CWIST_HTTP_MAX_BODY_SIZE) {
                                s->recv_xor ^= h2_xor_bytes(assembled, assembled_len);
                                cwist_sstring_append_len(s->req->body, (const char *)assembled, assembled_len);
                                sequence_complete = true;
                            }
                        }
                    }
                    if (!sequence_complete) {
                        /* TASFA-style ARQ boundary: the assembler retains the
                         * exact missing sequence numbers as recovery targets;
                         * HTTP/2 represents "retry this unprocessed request"
                         * with REFUSED_STREAM.  Never call the handler with a
                         * partial sequenced body. */
                        uint8_t rst[4] = {0, 0, 0, H2_ERR_REFUSED_STREAM};
                        h2_write_frame(hc.conn, CWIST_HTTP2_FRAME_RST_STREAM,
                                       0, stream_id, rst, sizeof(rst));
                        h2_auto_window_update(&hc, s);
                        h2_stream_remove(&hc, stream_id);
                        break;
                    }
                    cwist_http_response *res = cwist_http_response_create();
                    if (res) {
                        handler(user_ctx, s->req, res);
                        h2_inject_alt_svc(conn, res);
                        if (h2_send_response_hc(&hc, stream_id, res) != 0) connected = false;
                        cwist_http_response_destroy(res);
                    }
                    /* Keep the stream state alive until its final window
                     * credit is returned; h2_auto_window_update reads it. */
                    if (connected) h2_auto_window_update(&hc, s);
                    h2_stream_remove(&hc, stream_id);
                    s = NULL;
                }
                if (connected && s) {
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
                } else if (hc.ping_outstanding && memcmp(payload, hc.ping_payload, 8) == 0) {
                    /* RTT sample for the adaptive window/pacing tuner.  Keep
                     * one ping in flight while streams are active so the
                     * estimate tracks changing network conditions. */
                    uint64_t sample = h2_now_us() - hc.ping_sent_us;
                    hc.ping_outstanding = false;
                    if (sample > 0) cwist_http2_flow_control_update_rtt(&hc.fc, sample);
                    if (hc.streams) h2_send_ping(&hc);
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
