/**
 * @file http3.c
 * @brief lsquic/BoringSSL-based HTTP/3 server for CWIST.
 *
 * Implements a full HTTP/3 server using LiteSpeed's lsquic library
 * linked statically against BoringSSL.  Handles QUIC transport,
 * QPACK, and HTTP/3 framing per RFC 9114.
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/http3.h>
#include <cwist/net/http/http2.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/sys/err/cwist_err.h>
#include <cwist/sys/app/shutdown.h>
#include "tls_chain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <poll.h>
#include <pthread.h>
#ifdef __linux__
#include <sys/epoll.h>
#endif
#include <ctype.h>
#include <strings.h>
#include <inttypes.h>

#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>

#include <lsquic.h>
#ifdef CWIST_WEBTRANSPORT
#include <lsquic_wt.h>
#endif
#include <lsxpack_header.h>

/* ------------------------------------------------------------------ */
/* Globals                                                            */
/* ------------------------------------------------------------------ */

static pthread_mutex_t g_h3_global_mtx = PTHREAD_MUTEX_INITIALIZER;
static int             g_h3_global_ref = 0;

static void h3_global_init(void) {
    pthread_mutex_lock(&g_h3_global_mtx);
    if (g_h3_global_ref == 0) {
        lsquic_global_init(LSQUIC_GLOBAL_SERVER);
    }
    g_h3_global_ref++;
    pthread_mutex_unlock(&g_h3_global_mtx);
}

static void h3_global_cleanup(void) {
    pthread_mutex_lock(&g_h3_global_mtx);
    if (g_h3_global_ref > 0) {
        g_h3_global_ref--;
        if (g_h3_global_ref == 0) {
            lsquic_global_cleanup();
        }
    }
    pthread_mutex_unlock(&g_h3_global_mtx);
}

/* ------------------------------------------------------------------ */
/* Internal stream context                                            */
/* ------------------------------------------------------------------ */

typedef struct h3_stream_ctx {
    lsquic_stream_t *stream;
    cwist_http_request *req;
    cwist_http_response *res;
    char *body;
    size_t body_len;
    size_t body_cap;
    int headers_done;
    int response_ready;
    int write_state; /* 0=headers, 1=body, 2=done */
    size_t body_sent;
    uint8_t recv_xor; /* running XOR of received body bytes */
    uint8_t send_xor; /* running XOR of sent body bytes */
#ifdef CWIST_WEBTRANSPORT
    int is_webtransport;
    int wt_taken;
#endif
} h3_stream_ctx_t;

#ifdef CWIST_WEBTRANSPORT

typedef enum cwist_wt_handle_kind {
    CWIST_WT_HANDLE_SESSION = 1,
    CWIST_WT_HANDLE_STREAM = 2,
} cwist_wt_handle_kind_t;

typedef struct cwist_wt_handle {
    uint64_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t flags;
    void *ptr;
    struct cwist_wt_handle *parent;
    struct cwist_wt_handle *first_child;
    struct cwist_wt_handle *next_sibling;
} cwist_wt_handle_t;

#define CWIST_WT_HANDLE_MAGIC UINT64_C(0x4357495354575431)
#define CWIST_WT_HANDLE_VERSION 1

static cwist_wt_handle_t *cwist_wt_handle_new(cwist_wt_handle_kind_t kind,
                                              void *ptr) {
    if (!ptr) return NULL;
    cwist_wt_handle_t *handle = calloc(1, sizeof(*handle));
    if (!handle) return NULL;
    handle->magic = CWIST_WT_HANDLE_MAGIC;
    handle->version = CWIST_WT_HANDLE_VERSION;
    handle->kind = (uint16_t)kind;
    handle->ptr = ptr;
    return handle;
}

static void cwist_wt_handle_free(cwist_wt_handle_t *handle) {
    if (!handle) return;
    while (handle->first_child) {
        cwist_wt_handle_t *next = handle->first_child;
        handle->first_child = next->next_sibling;
        next->magic = 0;
        next->ptr = NULL;
        next->parent = NULL;
        next->first_child = NULL;
        next->next_sibling = NULL;
        free(next);
    }
    if (handle->parent) {
        cwist_wt_handle_t **link = &handle->parent->first_child;
        while (*link) {
            if (*link == handle) {
                *link = handle->next_sibling;
                break;
            }
            link = &(*link)->next_sibling;
        }
    }
    handle->magic = 0;
    handle->ptr = NULL;
    handle->parent = NULL;
    handle->first_child = NULL;
    handle->next_sibling = NULL;
    free(handle);
}

static void cwist_wt_handle_attach(cwist_wt_handle_t *parent,
                                   cwist_wt_handle_t *child) {
    if (!parent || !child) return;
    child->parent = parent;
    child->next_sibling = parent->first_child;
    parent->first_child = child;
}

static cwist_wt_handle_t *cwist_wt_handle_cast(void *handle,
                                               cwist_wt_handle_kind_t kind) {
    cwist_wt_handle_t *h = (cwist_wt_handle_t *)handle;
    if (!h || h->magic != CWIST_WT_HANDLE_MAGIC ||
        h->version != CWIST_WT_HANDLE_VERSION ||
        h->kind != (uint16_t)kind || !h->ptr) {
        return NULL;
    }
    return h;
}

static lsquic_stream_t *cwist_wt_handle_stream(void *handle) {
    cwist_wt_handle_t *h = cwist_wt_handle_cast(handle, CWIST_WT_HANDLE_STREAM);
    return h ? (lsquic_stream_t *)h->ptr : NULL;
}

static lsquic_wt_session_t *cwist_wt_handle_session(void *handle) {
    cwist_wt_handle_t *h = cwist_wt_handle_cast(handle, CWIST_WT_HANDLE_SESSION);
    return h ? (lsquic_wt_session_t *)h->ptr : NULL;
}

#endif /* CWIST_WEBTRANSPORT */

/* ------------------------------------------------------------------ */
/* Header-set interface for lsquic (QPACK decode)                     */
/* ------------------------------------------------------------------ */

#define H3_MAX_HEADERS 64
#define H3_DECODE_BUF_SIZE 65536

typedef struct cwist_h3_hset {
    lsquic_stream_t *stream;
    struct lsxpack_header headers[H3_MAX_HEADERS];
    size_t count;
    char decode_buf[H3_DECODE_BUF_SIZE];
    size_t decode_off;
} cwist_h3_hset_t;

static void *cwist_h3_hsi_create(void *hsi_ctx, lsquic_stream_t *stream,
                                 int is_push_promise) {
    (void)is_push_promise;
    cwist_h3_hset_t *hset = calloc(1, sizeof(*hset));
    if (!hset) return NULL;
    hset->stream = stream;
    return hset;
}

static struct lsxpack_header *
cwist_h3_hsi_prepare(void *hset_p, struct lsxpack_header *xhdr, size_t req_space) {
    cwist_h3_hset_t *hset = hset_p;
    if (xhdr) {
        /* Advance by the exact decoded size lsquic reports
         * (name_len + val_len + dec_overhead).  The old pointer-offset
         * arithmetic computed the wrong length and corrupted headers
         * when multiple Cookie values arrived in separate QPACK entries. */
        size_t total = lsxpack_header_get_dec_size(xhdr);
        if (total > sizeof(hset->decode_buf) - hset->decode_off)
            total = sizeof(hset->decode_buf) - hset->decode_off;
        hset->decode_off += total;
        if (hset->count < H3_MAX_HEADERS)
            hset->count++;
    }
    if (hset->count >= H3_MAX_HEADERS)
        return NULL;
    if (req_space > sizeof(hset->decode_buf) - hset->decode_off)
        return NULL;
    lsxpack_header_prepare_decode(&hset->headers[hset->count],
                                  hset->decode_buf, hset->decode_off,
                                  sizeof(hset->decode_buf) - hset->decode_off);
    return &hset->headers[hset->count];
}

static int cwist_h3_hsi_process_header(void *hset_p, struct lsxpack_header *xhdr) {
    (void)hset_p;
    (void)xhdr;
    return 0; /* success */
}

static void cwist_h3_hsi_discard(void *hset_p) {
    free(hset_p);
}

static const struct lsquic_hset_if cwist_h3_hset_if = {
    .hsi_create_header_set = cwist_h3_hsi_create,
    .hsi_prepare_decode    = cwist_h3_hsi_prepare,
    .hsi_process_header    = cwist_h3_hsi_process_header,
    .hsi_discard_header_set= cwist_h3_hsi_discard,
};

#ifdef CWIST_WEBTRANSPORT
static const struct lsquic_webtransport_if cwist_h3_wt_if;
#endif

/* ------------------------------------------------------------------ */
/* SSL context callback                                               */
/* ------------------------------------------------------------------ */

static SSL_CTX *cwist_h3_get_ssl_ctx(void *peer_ctx,
                                      const struct sockaddr *local) {
    (void)local;
    cwist_http3_context *h3_ctx = (cwist_http3_context *)peer_ctx;
    return h3_ctx ? h3_ctx->ssl_ctx : NULL;
}

/* ------------------------------------------------------------------ */
/* Packet-out callback                                                */
/* ------------------------------------------------------------------ */

static int cwist_h3_packets_out(void *ctx,
                                  const struct lsquic_out_spec *specs,
                                  unsigned n_specs) {
    int udp_fd = *(int *)ctx;
    unsigned i;
    for (i = 0; i < n_specs; ++i) {
        const struct lsquic_out_spec *spec = &specs[i];
        struct msghdr msg = {0};
        msg.msg_name = (void *)spec->dest_sa;
        msg.msg_namelen = (spec->dest_sa && spec->dest_sa->sa_family == AF_INET)
                          ? sizeof(struct sockaddr_in)
                          : sizeof(struct sockaddr_in6);
        msg.msg_iov = (struct iovec *)spec->iov;
        msg.msg_iovlen = spec->iovlen;
        ssize_t nw = sendmsg(udp_fd, &msg, MSG_DONTWAIT);
        if (nw < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            /* Non-fatal errors: log and continue if possible */
            if (errno == ECONNREFUSED || errno == ENETUNREACH ||
                errno == EHOSTUNREACH || errno == EMSGSIZE)
                continue;
            return -1;
        }
    }
    return (int)i;
}

/* ------------------------------------------------------------------ */
/* ALPN selection callback (BoringSSL)                                */
/* ------------------------------------------------------------------ */

static int cwist_h3_alpn_select_cb(SSL *ssl, const uint8_t **out, uint8_t *outlen,
                                    const uint8_t *in, unsigned inlen, void *arg) {
    (void)ssl;
    (void)arg;
    static const char *const protos[] = { "h3", "h3-29" };
    for (size_t p = 0; p < sizeof(protos) / sizeof(protos[0]); ++p) {
        const char *proto = protos[p];
        size_t plen = strlen(proto);
        const uint8_t *ptr = in;
        const uint8_t *end = in + inlen;
        while (ptr < end) {
            uint8_t len = *ptr++;
            if (ptr + len > end) break;
            if (len == plen && memcmp(ptr, proto, plen) == 0) {
                *out = ptr;
                *outlen = (uint8_t)plen;
                return SSL_TLSEXT_ERR_OK;
            }
            ptr += len;
        }
    }
    return SSL_TLSEXT_ERR_NOACK;
}

/* ------------------------------------------------------------------ */
/* Stream callbacks                                                   */
/* ------------------------------------------------------------------ */

static lsquic_conn_ctx_t *cwist_h3_on_new_conn(void *stream_if_ctx,
                                                lsquic_conn_t *conn) {
    cwist_http3_context *h3_ctx = stream_if_ctx;
    lsquic_conn_set_ctx(conn, (lsquic_conn_ctx_t *)h3_ctx);
    return (lsquic_conn_ctx_t *)h3_ctx;
}

static void cwist_h3_on_conn_closed(lsquic_conn_t *conn) {
    if (!conn) return;
    struct lsquic_conn_info info;
    if (lsquic_conn_get_info(conn, &info) == 0) {
        fprintf(stderr,
                "[HTTP/3] Conn closed rtt=%u rttvar=%u "
                "pkts_sent=%" PRIu64 " pkts_lost=%" PRIu64 " "
                "pkts_retx=%" PRIu64 " cwnd=%u\n",
                info.lci_rtt, info.lci_rttvar,
                info.lci_pkts_sent, info.lci_pkts_lost,
                info.lci_pkts_retx, info.lci_cwnd);
    }
}

static lsquic_stream_ctx_t *cwist_h3_on_new_stream(void *stream_if_ctx,
                                                    lsquic_stream_t *stream) {
    cwist_http3_context *h3_ctx = (cwist_http3_context *)stream_if_ctx;
    h3_stream_ctx_t *st = calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->stream = stream;
    st->req = cwist_http_request_create();
    if (!st->req) {
        free(st);
        return NULL;
    }
    cwist_sstring_assign(st->req->version, "HTTP/3");
    st->req->private_data = stream;
#ifdef CWIST_WEBTRANSPORT
    st->is_webtransport = 0;
#endif

    /* Handle server-pushed streams */
    if (h3_ctx && h3_ctx->push_enabled && lsquic_stream_is_pushed(stream)) {
        /* Pushed streams have their request headers already included
         * in the PUSH_PROMISE.  We process them the same way. */
        lsquic_stream_wantread(stream, 1);
        return (lsquic_stream_ctx_t *)st;
    }

    /* Default priority (middle of 1-256 range) */
    lsquic_stream_set_priority(stream, 128);
    lsquic_stream_wantread(stream, 1);

    return (lsquic_stream_ctx_t *)st;
}

static void h3_parse_path(cwist_http_request *req, const char *path) {
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

/* Lightweight XOR checksum over a byte buffer. */
static uint8_t h3_xor_bytes(const unsigned char *buf, size_t len) {
    uint8_t x = 0;
    for (size_t i = 0; i < len; i++) x ^= buf[i];
    return x;
}

static void h3_apply_header(cwist_http_request *req,
                            const char *name, const char *value) {
    if (strcmp(name, ":method") == 0) {
        req->method = cwist_http_string_to_method(value);
    } else if (strcmp(name, ":path") == 0) {
        h3_parse_path(req, value);
    } else if (strcmp(name, ":authority") == 0 || strcmp(name, "host") == 0) {
        cwist_http_header_add(&req->headers, "host", value);
    } else if (strcmp(name, ":scheme") == 0) {
        /* RFC 9114: silently ignore pseudo-headers we don't need to expose */
    } else if (strcmp(name, "content-length") == 0) {
        char *endptr = NULL;
        unsigned long long cl = strtoull(value, &endptr, 10);
        if (endptr && *endptr == '\0') {
            req->content_length = (size_t)cl;
        }
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "content-type") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "cookie") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "authorization") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "accept") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "user-agent") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "accept-encoding") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "accept-language") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "referer") == 0 || strcmp(name, "referrer") == 0) {
        cwist_http_header_add(&req->headers, "referer", value);
    } else if (strcmp(name, "origin") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "x-requested-with") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "priority") == 0) {
        /* RFC 9218 Extensible Priorities: u=urgency, i=incremental */
        cwist_http_header_add(&req->headers, name, value);
        /* Parse urgency value (u=N) where N is 0-7 */
        const char *u = strstr(value, "u=");
        if (u) {
            int urgency = atoi(u + 2);
            if (urgency >= 0 && urgency <= 7) {
                /* Map HTTP urgency (0=high, 7=low) to lsquic priority (1=high, 256=low) */
                unsigned pri = 1 + (unsigned)(urgency * 36);
                if (pri > 256) pri = 256;
                if (req->private_data) {
                    lsquic_stream_set_priority((lsquic_stream_t *)req->private_data, pri);
                }
            }
        }
    } else if (name[0] != ':') {
        /* Any other non-pseudo header */
        cwist_http_header_add(&req->headers, name, value);
    }
}

static void cwist_h3_on_read(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    h3_stream_ctx_t *st = (h3_stream_ctx_t *)st_h;
    if (!st) return;

    unsigned char buf[8192];
    ssize_t nread;

    if (!st->headers_done) {
        void *hset = lsquic_stream_get_hset(stream);
        if (hset) {
            cwist_h3_hset_t *hs = hset;
            size_t i;
            for (i = 0; i < hs->count; ++i) {
                const char *name  = lsxpack_header_get_name(&hs->headers[i]);
                const char *value = lsxpack_header_get_value(&hs->headers[i]);
                if (name && value) {
                    h3_apply_header(st->req, name, value);
#ifdef CWIST_WEBTRANSPORT
                    if (strcmp(name, ":protocol") == 0 && strcmp(value, "webtransport") == 0) {
                        /* WebTransport requires CONNECT method per RFC 9114 */
                        if (st->req && st->req->method == CWIST_HTTP_CONNECT) {
                            st->is_webtransport = 1;
                        }
                    }
#endif
                }
            }
            st->headers_done = 1;
        }
    }

    while ((nread = lsquic_stream_read(stream, buf, sizeof(buf))) > 0) {
        size_t need = st->body_len + (size_t)nread;
        if (need > CWIST_HTTP_MAX_BODY_SIZE) {
            /* Body too large: abort stream */
            lsquic_stream_close(stream);
            return;
        }
        if (need > st->body_cap) {
            size_t new_cap = st->body_cap ? st->body_cap * 2 : 4096;
            while (new_cap < need) new_cap *= 2;
            char *tmp = realloc(st->body, new_cap);
            if (!tmp) {
                lsquic_stream_close(stream);
                return;
            }
            st->body = tmp;
            st->body_cap = new_cap;
        }
        memcpy(st->body + st->body_len, buf, (size_t)nread);
        st->recv_xor ^= h3_xor_bytes((const unsigned char *)buf, (size_t)nread);
        st->body_len += (size_t)nread;
    }

    if (nread == 0) {
        /* End of stream (FIN received) */
        if (!st->headers_done) {
            /* Malformed request: no headers before FIN */
            st->res = cwist_http_response_create();
            if (st->res) {
                st->res->status_code = CWIST_HTTP_BAD_REQUEST;
            }
            st->response_ready = 1;
            lsquic_stream_wantread(stream, 0);
            lsquic_stream_wantwrite(stream, 1);
            return;
        }

        if (st->body_len > 0 && st->req && st->req->body) {
            cwist_sstring_assign_len(st->req->body, st->body, st->body_len);
        }

        st->res = cwist_http_response_create();
        if (st->res && st->req) {
            cwist_http3_context *h3_ctx = (cwist_http3_context *)
                lsquic_conn_get_ctx(lsquic_stream_conn(stream));
#ifdef CWIST_WEBTRANSPORT
            if (st->is_webtransport && h3_ctx && h3_ctx->wt_handler) {
                char *host = cwist_http_header_get(st->req->headers, "host");
                char *origin = cwist_http_header_get(st->req->headers, "origin");
                struct lsquic_wt_connect_info info = {
                    .wtci_authority = host,
                    .wtci_path = st->req->path ? st->req->path->data : NULL,
                    .wtci_origin = origin,
                    .wtci_protocol = NULL,
                    .wtci_draft = lsquic_wt_peer_draft(lsquic_stream_conn(stream)),
                };
                struct lsquic_wt_accept_params params = {
                    .wtap_status = LSQUIC_WTAP_STATUS_DEFAULT,
                    .wtap_wt_if = &cwist_h3_wt_if,
                    .wtap_wt_if_ctx = st,
                    .wtap_connect_info = &info,
                    .wtap_datagram_send_mode = LSQUIC_HTTP_DG_SEND_DEFAULT,
                };
                if (lsquic_wt_accept(stream, &params) == 0) {
                    st->wt_taken = 1;
                    lsquic_stream_wantread(stream, 0);
                    lsquic_stream_wantwrite(stream, 0);
                    return;
                }
            } else
#endif
            if (h3_ctx && h3_ctx->handler) {
                h3_ctx->handler(h3_ctx->user_ctx, st->req, st->res);
            }
        }
        st->response_ready = 1;
        lsquic_stream_wantread(stream, 0);
        lsquic_stream_wantwrite(stream, 1);
    } else if (nread < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        lsquic_stream_close(stream);
    }
}

static void cwist_h3_on_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    h3_stream_ctx_t *st = (h3_stream_ctx_t *)st_h;
    if (!st || !st->response_ready) return;

    if (st->write_state == 0) {
        struct lsxpack_header headers_arr[64];
        char hbuf[8192];
        size_t hbuf_off = 0;
        size_t hdr_count = 0;

        /* :status */
        char status_str[16];
        snprintf(status_str, sizeof(status_str), "%d", st->res->status_code);
        size_t slen = strlen(status_str);
        if (hdr_count < 64 && hbuf_off + 7 + 2 + slen <= sizeof(hbuf)) {
            memcpy(hbuf + hbuf_off, ":status", 7);
            memcpy(hbuf + hbuf_off + 9, status_str, slen);
            lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off,
                                       0, 7, 9, slen);
            hbuf_off += 9 + slen;
            hdr_count++;
        }

        /* content-length */
        size_t body_len = 0;
        if (st->res->use_file_stream) body_len = st->res->file_stream_len;
        else if (st->res->is_ptr_body) body_len = st->res->ptr_body_len;
        else if (st->res->body) body_len = st->res->body->size;

        if (body_len > 0 && hdr_count < 64) {
            char cl_str[32];
            snprintf(cl_str, sizeof(cl_str), "%zu", body_len);
            size_t cl_name_len = strlen("content-length");
            size_t cl_val_len  = strlen(cl_str);
            size_t total = cl_name_len + 2 + cl_val_len;
            if (hbuf_off + total <= sizeof(hbuf)) {
                memcpy(hbuf + hbuf_off, "content-length", cl_name_len);
                memcpy(hbuf + hbuf_off + cl_name_len + 2, cl_str, cl_val_len);
                lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off,
                                           0, cl_name_len, cl_name_len + 2, cl_val_len);
                hbuf_off += total;
                hdr_count++;
            }
        }

        /* content-type (if present) */
        if (st->res->headers) {
            char *ct = cwist_http_header_get(st->res->headers, "content-type");
            if (ct && hdr_count < 64) {
                size_t klen = strlen("content-type");
                size_t vlen = strlen(ct);
                if (hbuf_off + klen + 2 + vlen <= sizeof(hbuf)) {
                    memcpy(hbuf + hbuf_off, "content-type", klen);
                    memcpy(hbuf + hbuf_off + klen + 2, ct, vlen);
                    lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off,
                                               0, klen, klen + 2, vlen);
                    hbuf_off += klen + 2 + vlen;
                    hdr_count++;
                }
            }
        }

        /* user headers (skip content-length/content-type already handled) */
        cwist_http_header_node *node = st->res->headers;
        while (node && hdr_count < 64) {
            if (node->key && node->key->data && node->value && node->value->data) {
                /* Skip pseudo-headers and duplicates we already sent */
                if (node->key->data[0] == ':') {
                    node = node->next;
                    continue;
                }
                if (strncasecmp(node->key->data, "content-length", node->key->size) == 0 ||
                    strncasecmp(node->key->data, "content-type",   node->key->size) == 0) {
                    node = node->next;
                    continue;
                }
                size_t klen = node->key->size;
                size_t vlen = node->value->size;
                if (hbuf_off + klen + 2 + vlen <= sizeof(hbuf)) {
                    memcpy(hbuf + hbuf_off, node->key->data, klen);
                    memcpy(hbuf + hbuf_off + klen + 2, node->value->data, vlen);
                    lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off,
                                               0, klen, klen + 2, vlen);
                    hbuf_off += klen + 2 + vlen;
                    hdr_count++;
                }
            }
            node = node->next;
        }

        lsquic_http_headers_t headers = {
            .count = (unsigned)hdr_count,
            .headers = headers_arr,
        };
        int eos = (body_len == 0);
        if (lsquic_stream_send_headers(stream, &headers, eos) != 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                lsquic_stream_wantwrite(stream, 1);
                return;
            }
            lsquic_stream_close(stream);
            return;
        }
        st->write_state = 1;
        if (body_len == 0) {
            st->write_state = 2;
            lsquic_stream_wantwrite(stream, 0);
            return;
        }
    }

    if (st->write_state == 1 && st->res) {
        size_t body_len = 0;
        const char *body_data = NULL;

        if (st->res->use_file_stream) {
            /* Read file chunk into temporary buffer and write */
            if (st->res->file_stream_fd >= 0 && st->res->file_stream_len > 0) {
                static __thread char file_buf[65536];
                off_t offset = st->res->file_stream_offset + (off_t)st->body_sent;
                size_t to_read = st->res->file_stream_len - st->body_sent;
                if (to_read > sizeof(file_buf)) to_read = sizeof(file_buf);
                ssize_t nr = pread(st->res->file_stream_fd, file_buf, to_read, offset);
                if (nr > 0) {
                    ssize_t nw = lsquic_stream_write(stream, file_buf, (size_t)nr);
                    if (nw < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            lsquic_stream_wantwrite(stream, 1);
                            return;
                        }
                        lsquic_stream_close(stream);
                        return;
                    }
                    st->send_xor ^= h3_xor_bytes((const unsigned char *)file_buf, (size_t)nw);
                    st->body_sent += (size_t)nw;
                } else if (nr == 0) {
                    /* EOF: mark everything as sent */
                    st->body_sent = st->res->file_stream_len;
                } else if (nr < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        /* Retry next tick */
                    } else {
                        lsquic_stream_close(stream);
                        return;
                    }
                }
                body_len = st->res->file_stream_len;
            }
        } else if (st->res->is_ptr_body) {
            body_len = st->res->ptr_body_len;
            body_data = (const char *)st->res->ptr_body;
        } else if (st->res->body) {
            body_len = st->res->body->size;
            body_data = st->res->body->data;
        }

        if (body_data && body_len > 0 && st->body_sent < body_len) {
            ssize_t n = lsquic_stream_write(stream, body_data + st->body_sent,
                                            body_len - st->body_sent);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    lsquic_stream_wantwrite(stream, 1);
                    return;
                }
                lsquic_stream_close(stream);
                return;
            }
            st->send_xor ^= h3_xor_bytes((const unsigned char *)(body_data + st->body_sent), (size_t)n);
            st->body_sent += (size_t)n;
        }

        if (st->body_sent >= body_len) {
            st->write_state = 2;
            lsquic_stream_shutdown(stream, 1);
            lsquic_stream_wantwrite(stream, 0);
        }
    }
}

static void cwist_h3_on_close(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    h3_stream_ctx_t *st = (h3_stream_ctx_t *)st_h;
    if (st) {
        cwist_http_request_destroy(st->req);
        cwist_http_response_destroy(st->res);
        free(st->body);
        free(st);
    }
    (void)stream;
}

static struct {
    lsquic_conn_t *conn;
    char *data;
    size_t len;
} g_h3_dgram = {0};

static ssize_t cwist_h3_on_dg_write(lsquic_conn_t *conn, void *buf, size_t len) {
    if (g_h3_dgram.conn == conn && g_h3_dgram.data && g_h3_dgram.len > 0) {
        size_t to_copy = g_h3_dgram.len < len ? g_h3_dgram.len : len;
        memcpy(buf, g_h3_dgram.data, to_copy);
        free(g_h3_dgram.data);
        g_h3_dgram.data = NULL;
        g_h3_dgram.len = 0;
        g_h3_dgram.conn = NULL;
        lsquic_conn_want_datagram_write(conn, 0);
        return (ssize_t)to_copy;
    }
    return 0;
}

static void cwist_h3_on_datagram(lsquic_conn_t *conn, const void *buf, size_t len) {
    cwist_http3_context *ctx = (cwist_http3_context *)lsquic_conn_get_ctx(conn);
    if (ctx && ctx->datagram_cb) {
        ctx->datagram_cb(buf, len, ctx->datagram_user_ctx);
    }
}

#ifdef CWIST_WEBTRANSPORT

static lsquic_wt_session_ctx_t *
cwist_h3_wt_on_session_open(void *ctx, lsquic_wt_session_t *sess,
                            const struct lsquic_wt_connect_info *info) {
    (void)info;
    h3_stream_ctx_t *st = (h3_stream_ctx_t *)ctx;
    if (st && st->req && st->res) {
        lsquic_conn_t *conn = lsquic_wt_session_conn(sess);
        cwist_http3_context *h3_ctx = conn ? (cwist_http3_context *)lsquic_conn_get_ctx(conn) : NULL;
        cwist_wt_handle_t *session_handle =
            cwist_wt_handle_new(CWIST_WT_HANDLE_SESSION, sess);
        if (!session_handle) return NULL;
        if (h3_ctx && h3_ctx->wt_handler) {
            h3_ctx->wt_handler(st->req, st->res, session_handle);
        }
        return (lsquic_wt_session_ctx_t *)session_handle;
    }
    return NULL;
}

static void cwist_h3_wt_on_session_rejected(void *ctx,
                                            const struct lsquic_wt_connect_info *info,
                                            unsigned status, const char *reason,
                                            size_t reason_len) {
    (void)ctx;
    (void)info;
    (void)status;
    (void)reason;
    (void)reason_len;
}

static void cwist_h3_wt_on_session_close(lsquic_wt_session_t *sess,
                                         lsquic_wt_session_ctx_t *sess_ctx,
                                         uint64_t code, const char *reason,
                                         size_t reason_len) {
    (void)sess;
    (void)code;
    (void)reason;
    (void)reason_len;
    cwist_wt_handle_free((cwist_wt_handle_t *)sess_ctx);
}

static lsquic_stream_ctx_t *
cwist_h3_wt_on_stream(lsquic_wt_session_t *sess, lsquic_stream_t *stream) {
    cwist_http3_context *ctx = NULL;
    if (sess) {
        lsquic_conn_t *conn = lsquic_wt_session_conn(sess);
        if (conn) {
            ctx = (cwist_http3_context *)lsquic_conn_get_ctx(conn);
        }
    }
    if (ctx && ctx->wt_new_stream_handler) {
        cwist_wt_handle_t *stream_handle =
            cwist_wt_handle_new(CWIST_WT_HANDLE_STREAM, stream);
        if (!stream_handle) return NULL;
        ctx->wt_new_stream_handler(stream_handle, ctx->wt_new_stream_ctx);
        lsquic_stream_wantread(stream, 1);
        return (lsquic_stream_ctx_t *)stream_handle;
    }
    lsquic_stream_wantread(stream, 1);
    return (lsquic_stream_ctx_t *)cwist_wt_handle_new(CWIST_WT_HANDLE_STREAM, stream);
}

static lsquic_stream_ctx_t *
cwist_h3_wt_on_uni_stream(lsquic_wt_session_t *sess, lsquic_stream_t *stream) {
    return cwist_h3_wt_on_stream(sess, stream);
}

static lsquic_stream_ctx_t *
cwist_h3_wt_on_bidi_stream(lsquic_wt_session_t *sess, lsquic_stream_t *stream) {
    return cwist_h3_wt_on_stream(sess, stream);
}

static void cwist_h3_wt_on_stream_read(lsquic_stream_t *stream,
                                       lsquic_stream_ctx_t *st_h) {
    cwist_wt_handle_t *stream_handle = (cwist_wt_handle_t *)st_h;
    lsquic_wt_session_t *sess = lsquic_wt_session_from_stream(stream);
    if (!sess) return;
    lsquic_conn_t *conn = lsquic_wt_session_conn(sess);
    cwist_http3_context *ctx = conn ? (cwist_http3_context *)lsquic_conn_get_ctx(conn) : NULL;
    if (ctx && ctx->wt_new_stream_handler) {
        ctx->wt_new_stream_handler(stream_handle, ctx->wt_new_stream_ctx);
    }
}

static void cwist_h3_wt_on_stream_write(lsquic_stream_t *stream,
                                        lsquic_stream_ctx_t *st_h) {
    (void)stream;
    (void)st_h;
}

static void cwist_h3_wt_on_stream_close(lsquic_stream_t *stream,
                                        lsquic_stream_ctx_t *st_h) {
    (void)stream;
    cwist_wt_handle_free((cwist_wt_handle_t *)st_h);
}

static uint64_t cwist_h3_wt_on_stream_ss_code(lsquic_stream_t *stream,
                                              lsquic_stream_ctx_t *st_h) {
    (void)stream;
    (void)st_h;
    return 0;
}

static void cwist_h3_wt_on_datagram_read(lsquic_wt_session_t *sess,
                                         const void *buf, size_t len) {
    lsquic_conn_t *conn = lsquic_wt_session_conn(sess);
    cwist_http3_context *ctx = conn ? (cwist_http3_context *)lsquic_conn_get_ctx(conn) : NULL;
    if (ctx && ctx->datagram_cb) {
        ctx->datagram_cb(buf, len, ctx->datagram_user_ctx);
    }
}

static int cwist_h3_wt_on_datagram_write(lsquic_wt_session_t *sess,
                                         size_t max_datagram_size) {
    (void)sess;
    (void)max_datagram_size;
    return 0;
}

static const struct lsquic_webtransport_if cwist_h3_wt_if = {
    .wti_on_session_open    = cwist_h3_wt_on_session_open,
    .wti_on_session_rejected= cwist_h3_wt_on_session_rejected,
    .wti_on_session_close   = cwist_h3_wt_on_session_close,
    .wti_on_uni_stream      = cwist_h3_wt_on_uni_stream,
    .wti_on_bidi_stream     = cwist_h3_wt_on_bidi_stream,
    .wti_on_stream_read     = cwist_h3_wt_on_stream_read,
    .wti_on_stream_write    = cwist_h3_wt_on_stream_write,
    .wti_on_stream_close    = cwist_h3_wt_on_stream_close,
    .wti_on_stream_ss_code  = cwist_h3_wt_on_stream_ss_code,
    .wti_on_datagram_read   = cwist_h3_wt_on_datagram_read,
    .wti_on_datagram_write  = cwist_h3_wt_on_datagram_write,
};

#endif /* CWIST_WEBTRANSPORT */

static const struct lsquic_stream_if cwist_h3_stream_if = {
    .on_new_conn    = cwist_h3_on_new_conn,
    .on_conn_closed = cwist_h3_on_conn_closed,
    .on_new_stream  = cwist_h3_on_new_stream,
    .on_read        = cwist_h3_on_read,
    .on_write       = cwist_h3_on_write,
    .on_close       = cwist_h3_on_close,
    .on_dg_write    = cwist_h3_on_dg_write,
    .on_datagram    = cwist_h3_on_datagram,
};

/* ------------------------------------------------------------------ */
/* Context management                                                 */
/* ------------------------------------------------------------------ */

static int cwist_h3_ssl_ctx_init(SSL_CTX *ssl_ctx, int early_data) {
    SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ssl_ctx, TLS1_3_VERSION);
    SSL_CTX_set_default_verify_paths(ssl_ctx);
    SSL_CTX_set_alpn_select_cb(ssl_ctx, cwist_h3_alpn_select_cb, NULL);

    if (early_data) {
        SSL_CTX_set_early_data_enabled(ssl_ctx, 1);
    }

    /* Server-side QUIC transport parameters will be supplied by lsquic */
    return 0;
}

cwist_error_t cwist_http3_init_context(cwist_http3_context **ctx,
                                       const char *cert_path,
                                       const char *key_path) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!ctx || !cert_path || !key_path) {
        err.error.err_i16 = -1;
        return err;
    }

    h3_global_init();

    const SSL_METHOD *method = TLS_method();
    SSL_CTX *ssl_ctx = SSL_CTX_new(method);
    if (!ssl_ctx) {
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    cwist_h3_ssl_ctx_init(ssl_ctx, 0);

    if (SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_path) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ssl_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    if (cwist_tls_autoload_intermediates(ssl_ctx) < 0) {
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    *ctx = (cwist_http3_context *)cwist_alloc(sizeof(cwist_http3_context));
    if (!*ctx) {
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    memset(*ctx, 0, sizeof(cwist_http3_context));
    (*ctx)->ssl_ctx = ssl_ctx;
    (*ctx)->udp_fd = -1;
    err.error.err_i16 = 0;
    return err;
}

cwist_error_t cwist_http3_init_context_ephemeral(cwist_http3_context **ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!ctx) {
        err.error.err_i16 = -1;
        return err;
    }

    h3_global_init();

    const SSL_METHOD *method = TLS_method();
    SSL_CTX *ssl_ctx = SSL_CTX_new(method);
    if (!ssl_ctx) {
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    cwist_h3_ssl_ctx_init(ssl_ctx, 0);

    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!pctx) {
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }
    if (EVP_PKEY_keygen_init(pctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }
    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen(pctx, &pkey) <= 0 || !pkey) {
        EVP_PKEY_CTX_free(pctx);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }
    EVP_PKEY_CTX_free(pctx);

    X509 *x509 = X509_new();
    if (!x509) {
        EVP_PKEY_free(pkey);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 31536000L);
    X509_set_pubkey(x509, pkey);

    /* Self-signed subject */
    X509_NAME *subj = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(subj, "CN", MBSTRING_ASC,
                               (const unsigned char *)"localhost", -1, -1, 0);
    X509_set_issuer_name(x509, subj);
    X509_sign(x509, pkey, EVP_sha256());

    if (SSL_CTX_use_certificate(ssl_ctx, x509) != 1 ||
        SSL_CTX_use_PrivateKey(ssl_ctx, pkey) != 1) {
        X509_free(x509);
        EVP_PKEY_free(pkey);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    X509_free(x509);
    EVP_PKEY_free(pkey);

    *ctx = (cwist_http3_context *)cwist_alloc(sizeof(cwist_http3_context));
    if (!*ctx) {
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    memset(*ctx, 0, sizeof(cwist_http3_context));
    (*ctx)->ssl_ctx = ssl_ctx;
    (*ctx)->udp_fd = -1;
    err.error.err_i16 = 0;
    return err;
}

void cwist_http3_destroy_context(cwist_http3_context *ctx) {
    if (ctx) {
        if (ctx->engine) {
            lsquic_engine_destroy((lsquic_engine_t *)ctx->engine);
            ctx->engine = NULL;
        }
        if (ctx->ssl_ctx) {
            SSL_CTX_free(ctx->ssl_ctx);
            ctx->ssl_ctx = NULL;
        }
        cwist_free(ctx);
        h3_global_cleanup();
    }
}

/* ------------------------------------------------------------------ */
/* Server loop                                                        */
/* ------------------------------------------------------------------ */

cwist_error_t cwist_http3_server_loop(int udp_fd,
                                      cwist_http3_context *ctx,
                                      cwist_http3_request_handler_func handler,
                                      void *user_ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (udp_fd < 0 || !ctx || !ctx->ssl_ctx || !handler) {
        err.error.err_i16 = -1;
        return err;
    }

    struct lsquic_engine_settings settings;
    lsquic_engine_init_settings(&settings, LSENG_HTTP_SERVER);
    settings.es_versions = (1 << LSQVER_I001) | (1 << LSQVER_I002);
    settings.es_init_max_data = 1048576;
    settings.es_init_max_stream_data_bidi_local = 524288;
    settings.es_init_max_stream_data_bidi_remote = 524288;
    settings.es_max_streams_in = 100;
    settings.es_support_push = ctx->push_enabled;
    settings.es_allow_migration = ctx->allow_migration ? ctx->allow_migration : 1;
    settings.es_max_delayed_0rtt_packets = 32;
    settings.es_datagrams = ctx->datagram_enabled;
#ifdef CWIST_WEBTRANSPORT
    settings.es_http_datagrams = ctx->datagram_enabled || ctx->wt_handler != NULL;
    settings.es_webtransport = ctx->wt_handler != NULL;
    if (settings.es_webtransport) {
        settings.es_max_webtransport_sessions = 1;
        settings.es_reset_stream_at = 1;
    }
#endif
    settings.es_ecn = 1;
    settings.es_pace_packets = 1;
    settings.es_optimistic_nat = 1;

    if (ctx->idle_timeout_ms > 0)
        settings.es_idle_timeout = (unsigned)(ctx->idle_timeout_ms / 1000);
    if (ctx->handshake_timeout_ms > 0)
        settings.es_handshake_to = (unsigned long)ctx->handshake_timeout_ms * 1000UL;
    if (ctx->ping_period_ms > 0)
        settings.es_ping_period = (unsigned)(ctx->ping_period_ms / 1000);
    if (ctx->noprogress_timeout_ms > 0)
        settings.es_noprogress_timeout = (unsigned)(ctx->noprogress_timeout_ms / 1000);

    char err_buf[256];
    if (lsquic_engine_check_settings(&settings, LSENG_HTTP_SERVER,
                                     err_buf, sizeof(err_buf)) != 0) {
        fprintf(stderr, "[HTTP/3] Invalid engine settings: %s\n", err_buf);
        err.error.err_i16 = -1;
        return err;
    }

    struct lsquic_engine_api api = {
        .ea_stream_if        = &cwist_h3_stream_if,
        .ea_stream_if_ctx    = ctx,
        .ea_packets_out      = cwist_h3_packets_out,
        .ea_packets_out_ctx  = &udp_fd,
        .ea_get_ssl_ctx      = cwist_h3_get_ssl_ctx,
        .ea_hsi_if           = &cwist_h3_hset_if,
        .ea_hsi_ctx          = NULL,
        .ea_settings         = &settings,
        .ea_alpn             = "h3",
    };

    lsquic_engine_t *engine = lsquic_engine_new(LSENG_HTTP_SERVER, &api);
    if (!engine) {
        err.error.err_i16 = -1;
        return err;
    }

    ctx->engine = engine;
    ctx->handler = handler;
    ctx->user_ctx = user_ctx;
    ctx->udp_fd = udp_fd;
    ctx->running = 1;

    printf("[HTTP/3] Listening on UDP socket %d\n", udp_fd);

    struct sockaddr_storage local_addr;
    socklen_t local_addr_len = sizeof(local_addr);
    memset(&local_addr, 0, sizeof(local_addr));
    if (getsockname(udp_fd, (struct sockaddr *)&local_addr, &local_addr_len) != 0) {
        local_addr_len = 0;
    }

    /* Make socket non-blocking for polling */
    int flags = fcntl(udp_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(udp_fd, F_SETFL, flags | O_NONBLOCK);

    /* Enable ECN reception for congestion control feedback */
    int on = 1;
    setsockopt(udp_fd, IPPROTO_IP, IP_RECVTOS, &on, sizeof(on));
#ifdef IPV6_RECVTCLASS
    setsockopt(udp_fd, IPPROTO_IPV6, IPV6_RECVTCLASS, &on, sizeof(on));
#endif

    unsigned char *pkt_buf = malloc(65535);
    if (!pkt_buf) {
        lsquic_engine_destroy(engine);
        ctx->engine = NULL;
        err.error.err_i16 = -1;
        return err;
    }

#ifdef __linux__
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        err.error.err_i16 = -1;
        lsquic_engine_destroy(engine);
        ctx->engine = NULL;
        return err;
    }
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = udp_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, udp_fd, &ev) < 0) {
        close(epoll_fd);
        err.error.err_i16 = -1;
        lsquic_engine_destroy(engine);
        ctx->engine = NULL;
        return err;
    }
#endif

    while (ctx && ctx->running && atomic_load(&g_cwist_running)) {
        int diff = 1000; /* default 1 ms; let earliest_adv_tick drive it */
        if (lsquic_engine_earliest_adv_tick(engine, &diff)) {
            /* Enforce a small floor so pacing timers or back-to-back zero
             * ticks cannot turn this loop into a busy-wait. */
            if (diff < 1000)
                diff = 1000;
            else if (diff > 1000000)
                diff = 1000000;
        }

#ifdef __linux__
        struct epoll_event events[1];
        int pret = epoll_wait(epoll_fd, events, 1, diff / 1000);
        if (pret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pret > 0 && (events[0].events & (EPOLLERR | EPOLLHUP))) {
            fprintf(stderr, "[HTTP/3] UDP socket error, exiting loop.\n");
            break;
        }
        if (pret > 0 && (events[0].events & EPOLLIN)) {
#else
        struct pollfd pfd = { .fd = udp_fd, .events = POLLIN };
        int pret = poll(&pfd, 1, diff / 1000);

        if (pret < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EBADF) {
                fprintf(stderr, "[HTTP/3] UDP socket closed, exiting loop.\n");
                break;
            }
            /* Other fatal poll errors */
            break;
        }

        if (pret > 0) {
            if (pfd.revents & (POLLERR | POLLNVAL)) {
                fprintf(stderr, "[HTTP/3] UDP socket error, exiting loop.\n");
                break;
            }
            if (pfd.revents & POLLIN) {
#endif
                struct sockaddr_storage peer_addr;
                socklen_t peer_addr_len = sizeof(peer_addr);
                struct msghdr msg = {0};
                struct iovec iov = { pkt_buf, 65535 };
                msg.msg_name = &peer_addr;
                msg.msg_namelen = sizeof(peer_addr);
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;

                /* ECN support: allocate cmsg buffer */
                char cmsg_buf[CMSG_SPACE(sizeof(int))];
                msg.msg_control = cmsg_buf;
                msg.msg_controllen = sizeof(cmsg_buf);

                ssize_t nr = recvmsg(udp_fd, &msg, 0);
                if (nr > 0) {
                    int ecn = 0;
                    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
                         cmsg != NULL;
                         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                        if (cmsg->cmsg_level == IPPROTO_IP &&
                            cmsg->cmsg_type == IP_TOS) {
                            ecn = *(int *)CMSG_DATA(cmsg) & 0x3;
                            break;
                        }
#ifdef IPV6_TCLASS
                        if (cmsg->cmsg_level == IPPROTO_IPV6 &&
                            cmsg->cmsg_type == IPV6_TCLASS) {
                            ecn = *(int *)CMSG_DATA(cmsg) & 0x3;
                            break;
                        }
#endif
                    }
                    lsquic_engine_packet_in(engine, pkt_buf, (size_t)nr,
                                            local_addr_len ? (struct sockaddr *)&local_addr : NULL,
                                            (struct sockaddr *)&peer_addr,
                                            ctx, ecn);
                } else if (nr < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    if (errno == ECONNREFUSED || errno == ENETUNREACH ||
                        errno == EHOSTUNREACH) {
                        /* Transient error, keep going */
                    } else if (errno == EBADF) {
                        fprintf(stderr, "[HTTP/3] UDP socket closed.\n");
                        break;
                    }
                }
#ifdef __linux__
            }
#else
            }
        }
#endif

        lsquic_engine_process_conns(engine);
    }

#ifdef __linux__
    if (epoll_fd >= 0) close(epoll_fd);
#endif

    free(pkt_buf);
    if (ctx && ctx->engine) {
        lsquic_engine_destroy((lsquic_engine_t *)ctx->engine);
        ctx->engine = NULL;
    }
    err.error.err_i16 = 0;
    return err;
}

/* ------------------------------------------------------------------ */
/* Serve connection (kept for API compat)                             */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Push, Priority, and 0-RTT APIs                                     */
/* ------------------------------------------------------------------ */

void cwist_http3_set_push_enabled(cwist_http3_context *ctx, int enabled) {
    if (ctx) ctx->push_enabled = enabled;
}

int cwist_http3_push_resource(cwist_http_request *req,
                              const char *path,
                              const char *content_type) {
    if (!req || !req->private_data || !path) return -1;
    lsquic_stream_t *stream = (lsquic_stream_t *)req->private_data;
    lsquic_conn_t *conn = lsquic_stream_conn(stream);
    if (!conn || !lsquic_conn_is_push_enabled(conn)) return -1;

    /* Build push headers */
    struct lsxpack_header headers_arr[4];
    char hbuf[2048];
    size_t hbuf_off = 0;
    size_t hdr_count = 0;

    /* :method = GET */
    const char *method = "GET";
    size_t mlen = strlen(method);
    if (hbuf_off + 7 + 2 + mlen <= sizeof(hbuf)) {
        memcpy(hbuf + hbuf_off, ":method", 7);
        memcpy(hbuf + hbuf_off + 9, method, mlen);
        lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off,
                                   0, 7, 9, mlen);
        hbuf_off += 9 + mlen;
        hdr_count++;
    }

    /* :path */
    size_t plen = strlen(path);
    if (hbuf_off + 5 + 2 + plen <= sizeof(hbuf)) {
        memcpy(hbuf + hbuf_off, ":path", 5);
        memcpy(hbuf + hbuf_off + 7, path, plen);
        lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off,
                                   0, 5, 7, plen);
        hbuf_off += 7 + plen;
        hdr_count++;
    }

    /* :authority (copy from original request if available) */
    char *authority = cwist_http_header_get(req->headers, "host");
    if (!authority) authority = "localhost";
    size_t alen = strlen(authority);
    if (hbuf_off + 10 + 2 + alen <= sizeof(hbuf)) {
        memcpy(hbuf + hbuf_off, ":authority", 10);
        memcpy(hbuf + hbuf_off + 12, authority, alen);
        lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off,
                                   0, 10, 12, alen);
        hbuf_off += 12 + alen;
        hdr_count++;
    }

    /* content-type (optional) */
    if (content_type) {
        size_t ctlen = strlen(content_type);
        size_t ct_name_len = strlen("content-type");
        if (hbuf_off + ct_name_len + 2 + ctlen <= sizeof(hbuf)) {
            memcpy(hbuf + hbuf_off, "content-type", ct_name_len);
            memcpy(hbuf + hbuf_off + ct_name_len + 2, content_type, ctlen);
            lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off,
                                       0, ct_name_len, ct_name_len + 2, ctlen);
            hbuf_off += ct_name_len + 2 + ctlen;
            hdr_count++;
        }
    }

    lsquic_http_headers_t headers = {
        .count = (unsigned)hdr_count,
        .headers = headers_arr,
    };

    return lsquic_conn_push_stream(conn, NULL, stream, &headers);
}

int cwist_http3_set_stream_priority(cwist_http_request *req, unsigned priority) {
    if (!req || !req->private_data) return -1;
    if (priority < 1 || priority > 256) return -1;
    return lsquic_stream_set_priority((lsquic_stream_t *)req->private_data, priority);
}

/* ------------------------------------------------------------------ */
/* WebTransport API                                                   */
/* ------------------------------------------------------------------ */

void cwist_http3_set_webtransport_handler(cwist_http3_context *ctx,
                                          cwist_webtransport_handler_func handler) {
    if (ctx) ctx->wt_handler = handler;
}

void cwist_webtransport_set_new_stream_handler(cwist_http3_context *ctx,
                                               void (*handler)(void *stream, void *user_ctx),
                                               void *user_ctx) {
    if (ctx) {
        ctx->wt_new_stream_handler = handler;
        ctx->wt_new_stream_ctx = user_ctx;
    }
}

#ifdef CWIST_WEBTRANSPORT

ssize_t cwist_webtransport_read(void *stream, void *buf, size_t len) {
    if (!stream || !buf) return -1;
    lsquic_stream_t *s = cwist_wt_handle_stream(stream);
    if (!s) return -1;
    return lsquic_stream_read(s, buf, len);
}

ssize_t cwist_webtransport_write(void *stream, const void *data, size_t len) {
    if (!stream || !data) return -1;
    lsquic_stream_t *s = cwist_wt_handle_stream(stream);
    if (!s) return -1;
    return lsquic_stream_write(s, data, len);
}

int cwist_webtransport_flush(void *stream) {
    if (!stream) return -1;
    lsquic_stream_t *s = cwist_wt_handle_stream(stream);
    if (!s) return -1;
    return lsquic_stream_flush(s);
}

int cwist_webtransport_close_stream(void *stream) {
    if (!stream) return -1;
    lsquic_stream_t *s = cwist_wt_handle_stream(stream);
    if (!s) return -1;
    return lsquic_stream_close(s);
}

int cwist_webtransport_open_bidi_stream(void *session) {
    if (!session) return -1;
    cwist_wt_handle_t *session_handle =
        cwist_wt_handle_cast(session, CWIST_WT_HANDLE_SESSION);
    lsquic_wt_session_t *raw_session = session_handle ?
        (lsquic_wt_session_t *)session_handle->ptr : NULL;
    if (!raw_session) return -1;
    lsquic_stream_t *stream = lsquic_wt_open_bidi(raw_session);
    if (!stream) return -1;
    lsquic_wt_session_t *sess = lsquic_wt_session_from_stream(stream);
    if (sess) {
        lsquic_conn_t *conn = lsquic_wt_session_conn(sess);
        cwist_http3_context *ctx = conn ? (cwist_http3_context *)lsquic_conn_get_ctx(conn) : NULL;
        if (ctx && ctx->wt_new_stream_handler) {
            cwist_wt_handle_t *stream_handle =
                cwist_wt_handle_new(CWIST_WT_HANDLE_STREAM, stream);
            if (!stream_handle) return -1;
            cwist_wt_handle_attach(session_handle, stream_handle);
            ctx->wt_new_stream_handler(stream_handle, ctx->wt_new_stream_ctx);
        }
    }
    return 0;
}

int cwist_webtransport_open_uni_stream(void *session) {
    if (!session) return -1;
    cwist_wt_handle_t *session_handle =
        cwist_wt_handle_cast(session, CWIST_WT_HANDLE_SESSION);
    lsquic_wt_session_t *raw_session = session_handle ?
        (lsquic_wt_session_t *)session_handle->ptr : NULL;
    if (!raw_session) return -1;
    lsquic_stream_t *stream = lsquic_wt_open_uni(raw_session);
    if (!stream) return -1;
    lsquic_wt_session_t *sess = lsquic_wt_session_from_stream(stream);
    if (sess) {
        lsquic_conn_t *conn = lsquic_wt_session_conn(sess);
        cwist_http3_context *ctx = conn ? (cwist_http3_context *)lsquic_conn_get_ctx(conn) : NULL;
        if (ctx && ctx->wt_new_stream_handler) {
            cwist_wt_handle_t *stream_handle =
                cwist_wt_handle_new(CWIST_WT_HANDLE_STREAM, stream);
            if (!stream_handle) return -1;
            cwist_wt_handle_attach(session_handle, stream_handle);
            ctx->wt_new_stream_handler(stream_handle, ctx->wt_new_stream_ctx);
        }
    }
    return 0;
}

#else /* CWIST_WEBTRANSPORT */

ssize_t cwist_webtransport_read(void *stream, void *buf, size_t len) {
    (void)stream;
    (void)buf;
    (void)len;
    return -1;
}

ssize_t cwist_webtransport_write(void *stream, const void *data, size_t len) {
    (void)stream;
    (void)data;
    (void)len;
    return -1;
}

int cwist_webtransport_flush(void *stream) {
    (void)stream;
    return -1;
}

int cwist_webtransport_close_stream(void *stream) {
    (void)stream;
    return -1;
}

int cwist_webtransport_open_bidi_stream(void *session) {
    (void)session;
    return -1;
}

int cwist_webtransport_open_uni_stream(void *session) {
    (void)session;
    return -1;
}

#endif /* CWIST_WEBTRANSPORT */

/* ------------------------------------------------------------------ */
/* Resilience knobs                                                   */
/* ------------------------------------------------------------------ */

void cwist_http3_set_idle_timeout(cwist_http3_context *ctx, int ms) {
    if (ctx) ctx->idle_timeout_ms = ms > 0 ? ms : 0;
}

void cwist_http3_set_handshake_timeout(cwist_http3_context *ctx, int ms) {
    if (ctx) ctx->handshake_timeout_ms = ms > 0 ? ms : 0;
}

void cwist_http3_set_ping_period(cwist_http3_context *ctx, int ms) {
    if (ctx) ctx->ping_period_ms = ms > 0 ? ms : 0;
}

void cwist_http3_set_noprogress_timeout(cwist_http3_context *ctx, int ms) {
    if (ctx) ctx->noprogress_timeout_ms = ms > 0 ? ms : 0;
}

/* ------------------------------------------------------------------ */
/* Datagram API                                                       */
/* ------------------------------------------------------------------ */

void cwist_http3_set_datagram_enabled(cwist_http3_context *ctx, int enabled) {
    if (ctx) ctx->datagram_enabled = enabled;
}

void cwist_http3_set_datagram_callback(cwist_http3_context *ctx,
                                       void (*cb)(const void *data, size_t len, void *user_ctx),
                                       void *user_ctx) {
    if (ctx) {
        ctx->datagram_cb = cb;
        ctx->datagram_user_ctx = user_ctx;
    }
}

int cwist_http3_send_datagram(void *conn, const void *data, size_t len) {
    lsquic_conn_t *c = (lsquic_conn_t *)conn;
    if (!c || !data || len == 0) return -1;
    if (g_h3_dgram.data) free(g_h3_dgram.data);
    g_h3_dgram.conn = c;
    g_h3_dgram.data = malloc(len);
    if (!g_h3_dgram.data) return -1;
    memcpy(g_h3_dgram.data, data, len);
    g_h3_dgram.len = len;
    lsquic_conn_want_datagram_write(c, 1);
    return 0;
}

#ifdef CWIST_WEBTRANSPORT

ssize_t cwist_webtransport_send_datagram(void *session,
                                         const void *data, size_t len) {
    if (!session || !data || len == 0) return -1;
    lsquic_wt_session_t *sess = cwist_wt_handle_session(session);
    if (!sess) return -1;
    return lsquic_wt_send_datagram(sess, data, len);
}

size_t cwist_webtransport_max_datagram_size(void *session) {
    if (!session) return 0;
    lsquic_wt_session_t *sess = cwist_wt_handle_session(session);
    if (!sess) return 0;
    return lsquic_wt_max_datagram_size(sess);
}

int cwist_webtransport_close_session(void *session,
                                     uint64_t code,
                                     const char *reason) {
    if (!session) return -1;
    lsquic_wt_session_t *sess = cwist_wt_handle_session(session);
    if (!sess) return -1;
    return lsquic_wt_close(sess, code, reason,
                           reason ? strlen(reason) : 0);
}

#else /* CWIST_WEBTRANSPORT */

ssize_t cwist_webtransport_send_datagram(void *session,
                                         const void *data, size_t len) {
    (void)session;
    (void)data;
    (void)len;
    return -1;
}

size_t cwist_webtransport_max_datagram_size(void *session) {
    (void)session;
    return 0;
}

int cwist_webtransport_close_session(void *session,
                                     uint64_t code,
                                     const char *reason) {
    (void)session;
    (void)code;
    (void)reason;
    return -1;
}

#endif /* CWIST_WEBTRANSPORT */

/* ------------------------------------------------------------------ */
/* Serve connection (kept for API compat)                             */
/* ------------------------------------------------------------------ */

cwist_error_t cwist_http3_serve_connection(cwist_http3_connection *conn,
                                           void *user_ctx,
                                           cwist_http3_request_handler_func handler) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    (void)conn;
    (void)user_ctx;
    (void)handler;
    err.error.err_i16 = -1;
    return err;
}
