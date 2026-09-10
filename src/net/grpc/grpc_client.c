/**
 * @file grpc_client.c
 * @brief HTTP/2-based gRPC client (h2c cleartext, or TLS with ALPN "h2").
 *
 * The client speaks a minimal but complete HTTP/2 subset: client preface,
 * SETTINGS exchange, HEADERS/DATA framing with HPACK (encoding reuses the
 * shared h2_encode_* helpers from net/http/http2.c; decoding keeps a small
 * per-connection dynamic table), coarse flow control (WINDOW_UPDATE in both
 * directions), PING ACKs, and GOAWAY tolerance.  gRPC message framing
 * reuses cwist_grpc_encode_message / cwist_grpc_decoder from grpc.c.
 *
 * One active call per connection; the mutex serializes call setup.
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/net/grpc/grpc_client.h>
#include <cwist/core/mem/alloc.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <openssl/ssl.h>

#define GRPC_CLIENT_FRAME_HEADER 9
#define GRPC_CLIENT_MAX_FRAME (16u * 1024u * 1024u)
#define GRPC_CLIENT_DEFAULT_WINDOW 65535
#define GRPC_CLIENT_HEADER_TABLE 4096

#define H2_FRAME_DATA 0x00
#define H2_FRAME_HEADERS 0x01
#define H2_FRAME_RST_STREAM 0x03
#define H2_FRAME_SETTINGS 0x04
#define H2_FRAME_PUSH_PROMISE 0x05
#define H2_FRAME_PING 0x06
#define H2_FRAME_GOAWAY 0x07
#define H2_FRAME_WINDOW_UPDATE 0x08
#define H2_FRAME_CONTINUATION 0x09
#define H2_FLAG_END_STREAM 0x01
#define H2_FLAG_END_HEADERS 0x04
#define H2_FLAG_ACK 0x01

typedef struct grpc_client_hpack_entry {
    char *name;
    char *value;
    size_t size;
    struct grpc_client_hpack_entry *next;
} grpc_client_hpack_entry;

typedef struct grpc_client_msg {
    uint8_t *data;
    size_t len;
    struct grpc_client_msg *next;
} grpc_client_msg;

struct cwist_grpc_call {
    cwist_grpc_client *client;
    uint32_t stream_id;
    uint64_t deadline_ms;   /* monotonic; 0 = none */
    int64_t send_window;
    cwist_grpc_decoder decoder;
    grpc_client_msg *msg_head;
    grpc_client_msg *msg_tail;
    uint8_t *recv_buf;      /* backing store for the last returned message */
    cwist_grpc_status_t status;
    int status_seen;        /* grpc-status trailer/header arrived */
    char *status_message;
    int trailers;           /* trailer HEADERS seen */
    int stream_ended;       /* END_STREAM or RST_STREAM seen */
    int failed;
};

struct cwist_grpc_client {
    int fd;
    SSL *ssl;
    SSL_CTX *ssl_ctx;
    pthread_mutex_t mu;     /* one active call per connection */
    cwist_grpc_call *active;
    uint32_t next_stream_id;
    uint32_t peer_max_frame;
    uint32_t peer_initial_window;
    int64_t conn_send_window;
    grpc_client_hpack_entry *hpack_head;
    size_t hpack_size;
    size_t hpack_cap;
    int goaway;
    int dead;
    char *authority;
};

static uint64_t grpc_client_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Wait for readability/writability until the deadline; 0 ready, -1 error/timeout. */
static int grpc_client_wait(cwist_grpc_client *c, short events, uint64_t deadline_ms) {
    for (;;) {
        int timeout = -1;
        if (deadline_ms) {
            uint64_t now = grpc_client_now_ms();
            if (now >= deadline_ms) return -1;
            uint64_t left = deadline_ms - now;
            timeout = left > 600000 ? 600000 : (int)left;
        }
        struct pollfd pfd = { .fd = c->fd, .events = events };
        int rc = poll(&pfd, 1, timeout);
        if (rc > 0) {
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
            return 0;
        }
        if (rc == 0) return -1; /* timeout */
        if (errno == EINTR) continue;
        return -1;
    }
}

/* Read up to len bytes; returns the byte count, 0 on EOF, -1 on error/timeout. */
static ssize_t grpc_client_read_some(cwist_grpc_client *c, void *buf, size_t len,
                                     uint64_t deadline_ms) {
    for (;;) {
        ssize_t n;
        if (c->ssl) {
            n = SSL_read(c->ssl, buf, (int)len);
            if (n <= 0) {
                int err = SSL_get_error(c->ssl, (int)n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    if (grpc_client_wait(c, err == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN,
                                         deadline_ms) != 0)
                        return -1;
                    continue;
                }
                if (err == SSL_ERROR_ZERO_RETURN) return 0;
                return -1;
            }
            return n;
        }
        n = read(c->fd, buf, len);
        if (n > 0) return n;
        if (n == 0) return 0;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (grpc_client_wait(c, POLLIN, deadline_ms) != 0) return -1;
            continue;
        }
        return -1;
    }
}

static int grpc_client_read_all(cwist_grpc_client *c, void *buf, size_t len,
                                uint64_t deadline_ms) {
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = grpc_client_read_some(c, p + got, len - got, deadline_ms);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static int grpc_client_write_all(cwist_grpc_client *c, const void *buf, size_t len,
                                 uint64_t deadline_ms) {
    const uint8_t *p = buf;
    while (len) {
        ssize_t n;
        if (c->ssl) {
            n = SSL_write(c->ssl, p, (int)len);
            if (n <= 0) {
                int err = SSL_get_error(c->ssl, (int)n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    if (grpc_client_wait(c, err == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN,
                                         deadline_ms) != 0)
                        return -1;
                    continue;
                }
                return -1;
            }
        } else {
            n = write(c->fd, p, len);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (grpc_client_wait(c, POLLOUT, deadline_ms) != 0) return -1;
                    continue;
                }
                return -1;
            }
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int grpc_client_write_frame(cwist_grpc_client *c, uint8_t type, uint8_t flags,
                                   uint32_t stream_id, const void *payload, uint32_t len,
                                   uint64_t deadline_ms) {
    uint8_t hdr[GRPC_CLIENT_FRAME_HEADER];
    hdr[0] = (uint8_t)((len >> 16) & 0xff);
    hdr[1] = (uint8_t)((len >> 8) & 0xff);
    hdr[2] = (uint8_t)(len & 0xff);
    hdr[3] = type;
    hdr[4] = flags;
    hdr[5] = (uint8_t)((stream_id >> 24) & 0x7f);
    hdr[6] = (uint8_t)((stream_id >> 16) & 0xff);
    hdr[7] = (uint8_t)((stream_id >> 8) & 0xff);
    hdr[8] = (uint8_t)(stream_id & 0xff);
    if (grpc_client_write_all(c, hdr, sizeof(hdr), deadline_ms) != 0) return -1;
    if (len && grpc_client_write_all(c, payload, len, deadline_ms) != 0) return -1;
    return 0;
}

typedef struct grpc_client_frame {
    uint8_t type;
    uint8_t flags;
    uint32_t stream_id;
    uint32_t len;
    uint8_t *payload; /* owned; caller frees */
} grpc_client_frame;

static int grpc_client_read_frame(cwist_grpc_client *c, grpc_client_frame *f,
                                  uint64_t deadline_ms) {
    uint8_t hdr[GRPC_CLIENT_FRAME_HEADER];
    if (grpc_client_read_all(c, hdr, sizeof(hdr), deadline_ms) != 0) return -1;
    f->len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | hdr[2];
    f->type = hdr[3];
    f->flags = hdr[4];
    f->stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) | ((uint32_t)hdr[6] << 16) |
                   ((uint32_t)hdr[7] << 8) | hdr[8];
    if (f->len > GRPC_CLIENT_MAX_FRAME) return -1;
    f->payload = NULL;
    if (f->len) {
        f->payload = cwist_alloc(f->len);
        if (!f->payload) return -1;
        if (grpc_client_read_all(c, f->payload, f->len, deadline_ms) != 0) {
            cwist_free(f->payload);
            f->payload = NULL;
            return -1;
        }
    }
    return 0;
}

/* --- HPACK decoding (per-connection dynamic table) --- */

static void grpc_client_hpack_evict_to(cwist_grpc_client *c, size_t limit) {
    while (c->hpack_size > limit && c->hpack_head) {
        grpc_client_hpack_entry **pp = &c->hpack_head;
        while ((*pp)->next) pp = &(*pp)->next;
        grpc_client_hpack_entry *old = *pp;
        *pp = NULL;
        c->hpack_size -= old->size;
        cwist_free(old->name);
        cwist_free(old->value);
        cwist_free(old);
    }
}

static int grpc_client_hpack_insert(cwist_grpc_client *c, const char *name,
                                    const char *value) {
    size_t size = strlen(name) + strlen(value) + 32;
    if (size > c->hpack_cap) {
        grpc_client_hpack_evict_to(c, 0);
        return 0;
    }
    grpc_client_hpack_entry *e = cwist_alloc(sizeof(*e));
    if (!e) return -1;
    e->name = cwist_alloc(strlen(name) + 1);
    e->value = cwist_alloc(strlen(value) + 1);
    if (!e->name || !e->value) {
        cwist_free(e->name);
        cwist_free(e->value);
        cwist_free(e);
        return -1;
    }
    strcpy(e->name, name);
    strcpy(e->value, value);
    e->size = size;
    grpc_client_hpack_evict_to(c, c->hpack_cap - size);
    e->next = c->hpack_head;
    c->hpack_head = e;
    c->hpack_size += size;
    return 0;
}

/* Resolve a full HPACK index: 1..61 static, then dynamic (62 = newest).
 * Returned pointers are borrowed. */
static int grpc_client_hpack_get(cwist_grpc_client *c, uint32_t index,
                                 const char **name, const char **value) {
    const cwist_http2_static_header *st = h2_static_header(index);
    if (st) {
        *name = st->name;
        *value = st->value;
        return 0;
    }
    uint32_t pos = index - 61; /* 1-based into the dynamic table */
    const grpc_client_hpack_entry *e = c->hpack_head;
    while (e && --pos) e = e->next;
    if (!e) return -1;
    *name = e->name;
    *value = e->value;
    return 0;
}

typedef void (*grpc_client_header_cb)(void *ctx, const char *name, const char *value);

static int grpc_client_hpack_decode(cwist_grpc_client *c, const uint8_t *buf, size_t len,
                                    grpc_client_header_cb cb, void *ctx) {
    size_t pos = 0;
    while (pos < len) {
        uint8_t first = buf[pos];
        if (first & 0x80) { /* indexed */
            uint32_t index;
            if (h2_decode_integer(buf, len, &pos, 7, &index) != 0) return -1;
            const char *name, *value;
            if (grpc_client_hpack_get(c, index, &name, &value) != 0) return -1;
            cb(ctx, name, value);
        } else if (first & 0xc0) { /* 0x40: literal, incremental indexing */
            uint32_t name_index;
            if (h2_decode_integer(buf, len, &pos, 6, &name_index) != 0) return -1;
            char *owned_name = NULL;
            const char *name;
            if (name_index) {
                const char *unused;
                if (grpc_client_hpack_get(c, name_index, &name, &unused) != 0) return -1;
            } else {
                owned_name = h2_decode_string(buf, len, &pos);
                if (!owned_name) return -1;
                name = owned_name;
            }
            char *value = h2_decode_string(buf, len, &pos);
            if (!value) { cwist_free(owned_name); return -1; }
            if (grpc_client_hpack_insert(c, name, value) != 0) {
                cwist_free(owned_name);
                cwist_free(value);
                return -1;
            }
            cb(ctx, name, value);
            cwist_free(owned_name);
            cwist_free(value);
        } else if (first & 0x20) { /* dynamic table size update */
            uint32_t new_size;
            if (h2_decode_integer(buf, len, &pos, 5, &new_size) != 0) return -1;
            if (new_size > GRPC_CLIENT_HEADER_TABLE) return -1;
            c->hpack_cap = new_size;
            grpc_client_hpack_evict_to(c, new_size);
        } else { /* literal without indexing / never indexed */
            uint32_t name_index;
            if (h2_decode_integer(buf, len, &pos, 4, &name_index) != 0) return -1;
            char *owned_name = NULL;
            const char *name;
            if (name_index) {
                const char *unused;
                if (grpc_client_hpack_get(c, name_index, &name, &unused) != 0) return -1;
            } else {
                owned_name = h2_decode_string(buf, len, &pos);
                if (!owned_name) return -1;
                name = owned_name;
            }
            char *value = h2_decode_string(buf, len, &pos);
            if (!value) { cwist_free(owned_name); return -1; }
            cb(ctx, name, value);
            cwist_free(owned_name);
            cwist_free(value);
        }
    }
    return 0;
}

/* --- request header block encoding (literal, never-indexed) --- */

static size_t grpc_client_enc_literal(uint8_t *dst, size_t cap,
                                      uint32_t name_index, const char *name,
                                      const char *value) {
    size_t pos = 0;
    if (pos + 1 > cap) return 0;
    dst[pos] = 0x00;
    size_t n = h2_encode_integer(dst + pos, cap - pos, name_index, 4);
    if (n == 0) return 0;
    pos += n;
    if (!name_index) {
        n = h2_encode_string(dst + pos, cap - pos, name);
        if (n == 0) return 0;
        pos += n;
    }
    n = h2_encode_string(dst + pos, cap - pos, value);
    if (n == 0) return 0;
    return pos + n;
}

/* --- connection setup --- */

static int grpc_client_tcp_connect(const char *host, uint16_t port) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *it = res; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int grpc_client_tls_setup(cwist_grpc_client *c, const char *host,
                                 int verify_peer, uint64_t deadline_ms) {
    c->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!c->ssl_ctx) return -1;
    if (verify_peer) {
        SSL_CTX_set_verify(c->ssl_ctx, SSL_VERIFY_PEER, NULL);
        SSL_CTX_set_default_verify_paths(c->ssl_ctx);
    } else {
        SSL_CTX_set_verify(c->ssl_ctx, SSL_VERIFY_NONE, NULL);
    }
    static const uint8_t alpn[] = { 2, 'h', '2' };
    if (SSL_CTX_set_alpn_protos(c->ssl_ctx, alpn, sizeof(alpn)) != 0) return -1;
    c->ssl = SSL_new(c->ssl_ctx);
    if (!c->ssl) return -1;
    SSL_set_fd(c->ssl, c->fd);
    SSL_set_tlsext_host_name(c->ssl, host);
    for (;;) {
        int rc = SSL_connect(c->ssl);
        if (rc == 1) break;
        int err = SSL_get_error(c->ssl, rc);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            if (grpc_client_wait(c, err == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN,
                                 deadline_ms) != 0)
                return -1;
            continue;
        }
        return -1;
    }
    const uint8_t *sel = NULL;
    unsigned sel_len = 0;
    SSL_get0_alpn_selected(c->ssl, &sel, &sel_len);
    if (sel_len != 2 || memcmp(sel, "h2", 2) != 0) return -1;
    return 0;
}

/* Apply one server SETTINGS payload; answers with an ACK. */
static int grpc_client_apply_settings(cwist_grpc_client *c, const uint8_t *payload,
                                      uint32_t len, uint64_t deadline_ms) {
    if (len % 6 != 0) return -1;
    for (uint32_t off = 0; off + 6 <= len; off += 6) {
        uint16_t id = (uint16_t)((payload[off] << 8) | payload[off + 1]);
        uint32_t value = ((uint32_t)payload[off + 2] << 24) |
                         ((uint32_t)payload[off + 3] << 16) |
                         ((uint32_t)payload[off + 4] << 8) | payload[off + 5];
        switch (id) {
            case 0x2: /* ENABLE_PUSH: must be 0 for clients */
                if (value != 0) return -1;
                break;
            case 0x4: /* INITIAL_WINDOW_SIZE */
                if (value > 0x7fffffffu) return -1;
                if (c->active)
                    c->active->send_window += (int64_t)value - c->peer_initial_window;
                c->peer_initial_window = value;
                break;
            case 0x5: /* MAX_FRAME_SIZE */
                if (value < 16384 || value > 16777215) return -1;
                c->peer_max_frame = value;
                break;
            default:
                break;
        }
    }
    return grpc_client_write_frame(c, H2_FRAME_SETTINGS, H2_FLAG_ACK, 0, NULL, 0,
                                   deadline_ms);
}

cwist_grpc_client *cwist_grpc_client_connect(const char *host, uint16_t port,
                                             const cwist_grpc_client_options *options) {
    if (!host || !port) return NULL;
    cwist_grpc_client *c = cwist_alloc(sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    pthread_mutex_init(&c->mu, NULL);
    c->next_stream_id = 1;
    c->peer_max_frame = 16384;
    c->peer_initial_window = GRPC_CLIENT_DEFAULT_WINDOW;
    c->conn_send_window = GRPC_CLIENT_DEFAULT_WINDOW;
    c->hpack_cap = GRPC_CLIENT_HEADER_TABLE;

    uint64_t connect_ms = options && options->connect_timeout_ms
                              ? options->connect_timeout_ms : 10000;
    uint64_t deadline = grpc_client_now_ms() + connect_ms;

    if (options && options->authority) {
        c->authority = cwist_alloc(strlen(options->authority) + 1);
        if (c->authority) strcpy(c->authority, options->authority);
    } else {
        char buf[300];
        snprintf(buf, sizeof(buf), "%s:%u", host, (unsigned)port);
        c->authority = cwist_alloc(strlen(buf) + 1);
        if (c->authority) strcpy(c->authority, buf);
    }
    if (!c->authority) goto fail;

    c->fd = grpc_client_tcp_connect(host, port);
    if (c->fd < 0) goto fail;
    /* Non-blocking so poll() can enforce call deadlines. */
    int fl = fcntl(c->fd, F_GETFL, 0);
    if (fl < 0 || fcntl(c->fd, F_SETFL, fl | O_NONBLOCK) != 0) goto fail;

    if (options && options->use_tls) {
        if (grpc_client_tls_setup(c, host, options->verify_peer, deadline) != 0)
            goto fail;
    }

    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    if (grpc_client_write_all(c, preface, sizeof(preface) - 1, deadline) != 0)
        goto fail;
    if (grpc_client_write_frame(c, H2_FRAME_SETTINGS, 0, 0, NULL, 0, deadline) != 0)
        goto fail;

    /* The server's first frame must be SETTINGS; answer it, then keep
     * reading until our own SETTINGS ACK lands. */
    int server_settings = 0, our_ack = 0;
    while (!server_settings || !our_ack) {
        grpc_client_frame f;
        if (grpc_client_read_frame(c, &f, deadline) != 0) goto fail;
        int rc = 0;
        if (f.type == H2_FRAME_SETTINGS && !(f.flags & H2_FLAG_ACK)) {
            server_settings = 1;
            rc = grpc_client_apply_settings(c, f.payload, f.len, deadline);
        } else if (f.type == H2_FRAME_SETTINGS) {
            our_ack = 1;
        } else if (f.type == H2_FRAME_PING && !(f.flags & H2_FLAG_ACK) && f.len == 8) {
            rc = grpc_client_write_frame(c, H2_FRAME_PING, H2_FLAG_ACK, 0,
                                         f.payload, 8, deadline);
        } else if (f.type == H2_FRAME_GOAWAY) {
            cwist_free(f.payload);
            goto fail;
        }
        /* WINDOW_UPDATE and friends before the first call need no action. */
        cwist_free(f.payload);
        if (rc != 0) goto fail;
    }
    return c;

fail:
    cwist_grpc_client_close(c);
    return NULL;
}

void cwist_grpc_client_close(cwist_grpc_client *c) {
    if (!c) return;
    if (c->active) cwist_grpc_call_destroy(c->active);
    if (c->fd >= 0 && !c->dead)
        grpc_client_write_frame(c, H2_FRAME_GOAWAY, 0, 0, NULL, 0,
                                grpc_client_now_ms() + 500);
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
    }
    if (c->ssl_ctx) SSL_CTX_free(c->ssl_ctx);
    if (c->fd >= 0) close(c->fd);
    grpc_client_hpack_evict_to(c, 0);
    pthread_mutex_destroy(&c->mu);
    cwist_free(c->authority);
    cwist_free(c);
}

/* --- call plumbing --- */

static int grpc_client_queue_msg(void *ctx, const cwist_grpc_message *message) {
    cwist_grpc_call *call = ctx;
    grpc_client_msg *node = cwist_alloc(sizeof(*node));
    if (!node) return -1;
    node->data = cwist_alloc(message->len ? message->len : 1);
    if (!node->data) {
        cwist_free(node);
        return -1;
    }
    if (message->len) memcpy(node->data, message->data, message->len);
    node->len = message->len;
    node->next = NULL;
    if (call->msg_tail) call->msg_tail->next = node;
    else call->msg_head = node;
    call->msg_tail = node;
    return 0;
}

typedef struct grpc_client_headers_ctx {
    cwist_grpc_call *call;
    int http_status;
} grpc_client_headers_ctx;

static void grpc_client_on_header(void *ctx, const char *name, const char *value) {
    grpc_client_headers_ctx *hc = ctx;
    if (strcmp(name, ":status") == 0) {
        hc->http_status = atoi(value);
    } else if (strcmp(name, "grpc-status") == 0) {
        hc->call->status = (cwist_grpc_status_t)atoi(value);
        hc->call->status_seen = 1;
    } else if (strcmp(name, "grpc-message") == 0) {
        cwist_free(hc->call->status_message);
        hc->call->status_message = cwist_alloc(strlen(value) + 1);
        if (hc->call->status_message) strcpy(hc->call->status_message, value);
    }
}

/* Read one header block (HEADERS + any CONTINUATION) for the call stream.
 * Consumes the already-read first frame. */
static int grpc_client_read_header_block(cwist_grpc_client *c, cwist_grpc_call *call,
                                         grpc_client_frame *first,
                                         uint8_t **out, size_t *out_len,
                                         int *end_stream, uint64_t deadline_ms) {
    size_t cap = first->len ? first->len : 1;
    uint8_t *block = cwist_alloc(cap);
    if (!block) return -1;
    size_t len = first->len;
    if (len) memcpy(block, first->payload, len);
    *end_stream = !!(first->flags & H2_FLAG_END_STREAM);
    while (!(first->flags & H2_FLAG_END_HEADERS)) {
        grpc_client_frame cont;
        if (grpc_client_read_frame(c, &cont, deadline_ms) != 0 ||
            cont.type != H2_FRAME_CONTINUATION || cont.stream_id != call->stream_id) {
            cwist_free(block);
            return -1;
        }
        uint8_t *nb = cwist_realloc(block, len + cont.len);
        if (!nb) {
            cwist_free(cont.payload);
            cwist_free(block);
            return -1;
        }
        block = nb;
        if (cont.len) memcpy(block + len, cont.payload, cont.len);
        len += cont.len;
        first->flags = cont.flags; /* CONTINUATION carries END_HEADERS */
        cwist_free(cont.payload);
    }
    *out = block;
    *out_len = len;
    return 0;
}

/* Process one inbound frame during a call.  Returns 0 to keep pumping,
 * 1 when the call stream ended (trailers or RST), -1 on error/deadline. */
static int grpc_client_pump(cwist_grpc_call *call) {
    cwist_grpc_client *c = call->client;
    if (call->deadline_ms && grpc_client_now_ms() >= call->deadline_ms)
        goto deadline;
    grpc_client_frame f;
    if (grpc_client_read_frame(c, &f, call->deadline_ms) != 0) {
        if (call->deadline_ms && grpc_client_now_ms() >= call->deadline_ms)
            goto deadline;
        c->dead = 1;
        goto transport_fail;
    }

    int rc = 0;
    switch (f.type) {
        case H2_FRAME_SETTINGS:
            if (!(f.flags & H2_FLAG_ACK))
                rc = grpc_client_apply_settings(c, f.payload, f.len, call->deadline_ms);
            break;
        case H2_FRAME_PING:
            if (!(f.flags & H2_FLAG_ACK) && f.len == 8)
                rc = grpc_client_write_frame(c, H2_FRAME_PING, H2_FLAG_ACK, 0,
                                             f.payload, 8, call->deadline_ms);
            break;
        case H2_FRAME_WINDOW_UPDATE: {
            if (f.len != 4) { rc = -1; break; }
            int32_t inc = (int32_t)((((uint32_t)f.payload[0] & 0x7f) << 24) |
                                    ((uint32_t)f.payload[1] << 16) |
                                    ((uint32_t)f.payload[2] << 8) | f.payload[3]);
            if (inc <= 0) { rc = -1; break; }
            if (f.stream_id == 0) c->conn_send_window += inc;
            else if (f.stream_id == call->stream_id) call->send_window += inc;
            break;
        }
        case H2_FRAME_GOAWAY:
            c->goaway = 1;
            if (!call->trailers) {
                call->failed = 1;
                call->stream_ended = 1;
                call->status = CWIST_GRPC_UNAVAILABLE;
                cwist_free(call->status_message);
                call->status_message = NULL;
                rc = 1;
            }
            break;
        case H2_FRAME_RST_STREAM:
            if (f.stream_id == call->stream_id) {
                call->stream_ended = 1;
                if (!call->trailers) {
                    call->failed = 1;
                    call->status = CWIST_GRPC_UNAVAILABLE;
                }
                rc = 1;
            }
            break;
        case H2_FRAME_HEADERS: {
            if (f.stream_id != call->stream_id) break;
            uint8_t *block = NULL;
            size_t block_len = 0;
            int end_stream = 0;
            if (grpc_client_read_header_block(c, call, &f, &block, &block_len,
                                              &end_stream, call->deadline_ms) != 0) {
                rc = -1;
                break;
            }
            grpc_client_headers_ctx hc = { call, 0 };
            if (grpc_client_hpack_decode(c, block, block_len,
                                         grpc_client_on_header, &hc) != 0) {
                cwist_free(block);
                rc = -1;
                break;
            }
            cwist_free(block);
            if (hc.http_status && hc.http_status != 200) {
                call->failed = 1;
                call->stream_ended = 1;
                call->status = CWIST_GRPC_UNAVAILABLE;
                cwist_free(call->status_message);
                call->status_message = cwist_alloc(32);
                if (call->status_message)
                    snprintf(call->status_message, 32, "HTTP status %d", hc.http_status);
                rc = 1;
                break;
            }
            if (end_stream) {
                /* Trailer HEADERS close the stream; a missing grpc-status
                 * is a protocol error surfaced as UNKNOWN (gRPC spec). */
                call->trailers = 1;
                call->stream_ended = 1;
                if (!call->status_seen) call->status = CWIST_GRPC_UNKNOWN;
                rc = 1;
            }
            break;
        }
        case H2_FRAME_DATA: {
            if (f.stream_id != call->stream_id) break;
            if (f.len) {
                if (cwist_grpc_decoder_feed(&call->decoder, f.payload, f.len,
                                            grpc_client_queue_msg, call) != 0) {
                    call->failed = 1;
                    call->stream_ended = 1;
                    call->status = CWIST_GRPC_INTERNAL;
                    cwist_free(call->status_message);
                    call->status_message = cwist_alloc(32);
                    if (call->status_message)
                        strcpy(call->status_message, "malformed gRPC message frame");
                    rc = 1;
                    break;
                }
                /* Return flow-control credit for what we consumed. */
                uint8_t upd[4];
                upd[0] = (uint8_t)((f.len >> 24) & 0x7f);
                upd[1] = (uint8_t)(f.len >> 16);
                upd[2] = (uint8_t)(f.len >> 8);
                upd[3] = (uint8_t)f.len;
                if (grpc_client_write_frame(c, H2_FRAME_WINDOW_UPDATE, 0, 0,
                                            upd, 4, call->deadline_ms) != 0 ||
                    grpc_client_write_frame(c, H2_FRAME_WINDOW_UPDATE, 0,
                                            call->stream_id, upd, 4,
                                            call->deadline_ms) != 0) {
                    rc = -1;
                    break;
                }
            }
            if (f.flags & H2_FLAG_END_STREAM) {
                /* END_STREAM on DATA without trailers: tolerate, status stays
                 * UNKNOWN unless trailers arrive... they cannot; mark done. */
                call->stream_ended = 1;
                if (!call->trailers) {
                    call->trailers = 1;
                    call->status = CWIST_GRPC_UNKNOWN;
                }
                rc = 1;
            }
            break;
        }
        default:
            break; /* PUSH_PROMISE, PRIORITY, unknown: ignore */
    }
    cwist_free(f.payload);
    return rc;

deadline:
    call->failed = 1;
    call->stream_ended = 1;
    call->status = CWIST_GRPC_DEADLINE_EXCEEDED;
    cwist_free(call->status_message);
    call->status_message = cwist_alloc(32);
    if (call->status_message) strcpy(call->status_message, "deadline exceeded");
    grpc_client_write_frame(c, H2_FRAME_RST_STREAM, 0, call->stream_id,
                            (uint8_t[]){0, 0, 0, 0x08}, 4,
                            grpc_client_now_ms() + 500);
    return -1;

transport_fail:
    call->failed = 1;
    call->stream_ended = 1;
    call->status = CWIST_GRPC_UNAVAILABLE;
    cwist_free(call->status_message);
    call->status_message = cwist_alloc(32);
    if (call->status_message) strcpy(call->status_message, "transport error");
    return -1;
}

/* Send the framed request message, honouring flow control coarsely: pump
 * inbound frames while the peer's windows are too small. */
static int grpc_client_send_request(cwist_grpc_call *call, const uint8_t *frame,
                                    size_t frame_len) {
    cwist_grpc_client *c = call->client;
    size_t sent = 0;
    for (;;) {
        int64_t window = c->conn_send_window < call->send_window
                             ? c->conn_send_window : call->send_window;
        size_t chunk = frame_len - sent;
        if ((int64_t)chunk > window) chunk = window > 0 ? (size_t)window : 0;
        if (chunk > c->peer_max_frame) chunk = c->peer_max_frame;
        if (chunk == 0) {
            if (frame_len == sent) break;
            int rc = grpc_client_pump(call);
            if (rc != 0) return -1;
            continue;
        }
        uint8_t flags = (sent + chunk == frame_len) ? H2_FLAG_END_STREAM : 0;
        if (grpc_client_write_frame(c, H2_FRAME_DATA, flags, call->stream_id,
                                    frame + sent, (uint32_t)chunk,
                                    call->deadline_ms) != 0)
            return -1;
        c->conn_send_window -= (int64_t)chunk;
        call->send_window -= (int64_t)chunk;
        sent += chunk;
        if (sent == frame_len) break;
    }
    return 0;
}

cwist_grpc_call *cwist_grpc_call_start(cwist_grpc_client *c, const char *method,
                                       const void *request, size_t request_len,
                                       uint64_t timeout_ms) {
    if (!c || !method || (request_len && !request) || c->dead || c->goaway)
        return NULL;

    pthread_mutex_lock(&c->mu);
    if (c->active) {
        pthread_mutex_unlock(&c->mu);
        return NULL;
    }

    cwist_grpc_call *call = cwist_alloc(sizeof(*call));
    if (!call) {
        pthread_mutex_unlock(&c->mu);
        return NULL;
    }
    memset(call, 0, sizeof(*call));
    call->client = c;
    call->stream_id = c->next_stream_id;
    c->next_stream_id += 2;
    call->deadline_ms = timeout_ms ? grpc_client_now_ms() + timeout_ms : 0;
    call->send_window = c->peer_initial_window;
    call->status = CWIST_GRPC_OK;
    cwist_grpc_decoder_init(&call->decoder, 0);

    /* HEADERS: :method POST, :scheme, :path, :authority, content-type, te. */
    uint8_t block[4096];
    size_t pos = 0;
    block[pos++] = 0x83; /* :method: POST */
    block[pos++] = c->ssl ? 0x87 : 0x86; /* :scheme: https / http */
    size_t n = grpc_client_enc_literal(block + pos, sizeof(block) - pos, 4, NULL, method);
    if (n == 0) goto fail;
    pos += n;
    n = grpc_client_enc_literal(block + pos, sizeof(block) - pos, 1, NULL, c->authority);
    if (n == 0) goto fail;
    pos += n;
    n = grpc_client_enc_literal(block + pos, sizeof(block) - pos, 31, NULL,
                                "application/grpc");
    if (n == 0) goto fail;
    pos += n;
    n = grpc_client_enc_literal(block + pos, sizeof(block) - pos, 0, "te", "trailers");
    if (n == 0) goto fail;
    pos += n;
    if (timeout_ms) {
        char timeout_hdr[16];
        uint64_t v = timeout_ms > 99999999u ? 99999999u : timeout_ms;
        snprintf(timeout_hdr, sizeof(timeout_hdr), "%llum", (unsigned long long)v);
        n = grpc_client_enc_literal(block + pos, sizeof(block) - pos, 0,
                                    "grpc-timeout", timeout_hdr);
        if (n == 0) goto fail;
        pos += n;
    }
    if (grpc_client_write_frame(c, H2_FRAME_HEADERS, H2_FLAG_END_HEADERS,
                                call->stream_id, block, (uint32_t)pos,
                                call->deadline_ms) != 0)
        goto fail;

    uint8_t *frame = NULL;
    size_t frame_len = 0;
    if (cwist_grpc_encode_message(request, request_len, 0, &frame, &frame_len) != 0)
        goto fail;
    int rc = grpc_client_send_request(call, frame, frame_len);
    cwist_free(frame);
    if (rc != 0) goto fail;

    c->active = call;
    pthread_mutex_unlock(&c->mu);
    return call;

fail:
    cwist_grpc_decoder_destroy(&call->decoder);
    cwist_free(call);
    pthread_mutex_unlock(&c->mu);
    return NULL;
}

int cwist_grpc_call_recv(cwist_grpc_call *call, cwist_grpc_message *out) {
    if (!call || !out) return -1;
    while (!call->msg_head) {
        if (call->stream_ended) return call->failed ? -1 : 0;
        if (grpc_client_pump(call) != 0 && !call->msg_head)
            return call->failed ? -1 : 0;
    }
    grpc_client_msg *node = call->msg_head;
    call->msg_head = node->next;
    if (!call->msg_head) call->msg_tail = NULL;
    cwist_free(call->recv_buf);
    call->recv_buf = node->data;
    out->compressed = 0;
    out->data = node->data;
    out->len = node->len;
    cwist_free(node);
    return 1;
}

cwist_grpc_status_t cwist_grpc_call_finish(cwist_grpc_call *call, const char **message) {
    if (!call) {
        if (message) *message = NULL;
        return CWIST_GRPC_INTERNAL;
    }
    while (!call->stream_ended) {
        if (grpc_client_pump(call) != 0 && !call->stream_ended)
            break;
    }
    if (message) *message = call->status_message;
    return call->status;
}

void cwist_grpc_call_cancel(cwist_grpc_call *call) {
    if (!call || call->stream_ended) return;
    cwist_grpc_client *c = call->client;
    grpc_client_write_frame(c, H2_FRAME_RST_STREAM, 0, call->stream_id,
                            (uint8_t[]){0, 0, 0, 0x08}, 4,
                            grpc_client_now_ms() + 500);
    call->stream_ended = 1;
    call->failed = 1;
    call->status = CWIST_GRPC_CANCELLED;
}

void cwist_grpc_call_destroy(cwist_grpc_call *call) {
    if (!call) return;
    cwist_grpc_client *c = call->client;
    if (!call->stream_ended) cwist_grpc_call_cancel(call);
    pthread_mutex_lock(&c->mu);
    if (c->active == call) c->active = NULL;
    pthread_mutex_unlock(&c->mu);
    cwist_grpc_decoder_destroy(&call->decoder);
    while (call->msg_head) {
        grpc_client_msg *next = call->msg_head->next;
        cwist_free(call->msg_head->data);
        cwist_free(call->msg_head);
        call->msg_head = next;
    }
    cwist_free(call->recv_buf);
    cwist_free(call->status_message);
    cwist_free(call);
}
