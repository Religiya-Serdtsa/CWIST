/**
 * @file http3_client.c
 * @brief lsquic/BoringSSL-based HTTP/3 client for CWIST.
 */

#if defined(__APPLE__)
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#elif !defined(__FreeBSD__) && !defined(__NetBSD__) && !defined(__OpenBSD__) && !defined(__DragonFly__)
#define _POSIX_C_SOURCE 200809L
#endif
#include <cwist/net/http/http3_client.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/sys/err/cwist_err.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <poll.h>
#include <pthread.h>
#include <ctype.h>
#include <strings.h>

#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/evp.h>

#include <lsquic.h>
#ifdef CWIST_WEBTRANSPORT
#include <lsquic_wt.h>
#endif
#include <lsxpack_header.h>

/* BSD sockets do not universally provide MSG_DONTWAIT.  This client creates
 * its UDP socket in non-blocking mode before lsquic can emit packets. */
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

/* ------------------------------------------------------------------ */
/* Globals                                                            */
/* ------------------------------------------------------------------ */

static pthread_mutex_t g_h3c_global_mtx = PTHREAD_MUTEX_INITIALIZER;
static int             g_h3c_global_ref = 0;

static void h3c_global_init(void) {
    pthread_mutex_lock(&g_h3c_global_mtx);
    if (g_h3c_global_ref == 0) {
        lsquic_global_init(LSQUIC_GLOBAL_CLIENT);
    }
    g_h3c_global_ref++;
    pthread_mutex_unlock(&g_h3c_global_mtx);
}

static void h3c_global_cleanup(void) {
    pthread_mutex_lock(&g_h3c_global_mtx);
    if (g_h3c_global_ref > 0) {
        g_h3c_global_ref--;
        if (g_h3c_global_ref == 0) {
            lsquic_global_cleanup();
        }
    }
    pthread_mutex_unlock(&g_h3c_global_mtx);
}

/* ------------------------------------------------------------------ */
/* Client handle                                                      */
/* ------------------------------------------------------------------ */

typedef struct h3c_stream_ctx {
    lsquic_stream_t *stream;
    cwist_http_response *res;
    char *req_path;
    cwist_http_method_t req_method;
    cwist_http_header_node *req_headers;
    char *req_body;
    size_t req_body_len;
    size_t req_body_sent;
    char *resp_body;
    size_t resp_body_len;
    size_t resp_body_cap;
    int headers_done;
    int response_ready;
    int write_done;
#ifdef CWIST_WEBTRANSPORT
    int is_webtransport_connect;
#endif
} h3c_stream_ctx_t;

#ifdef CWIST_WEBTRANSPORT
struct cwist_webtransport_client_session {
    lsquic_wt_session_t *native;
    struct cwist_http3_client *client;
    int open;
    int rejected;
};
#endif

struct cwist_http3_client {
    SSL_CTX *ssl_ctx;
    lsquic_engine_t *engine;
    lsquic_conn_t *conn;
    int udp_fd;
    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len;
    struct sockaddr_storage local_addr;
    socklen_t local_addr_len;
    char *host;
    uint16_t port;
    int timeout_ms;
    int datagram_enabled;
    int max_retries;
    int retry_delay_ms;
    int conn_timeout_ms;

    pthread_mutex_t mtx;
    pthread_cond_t cond;
    h3c_stream_ctx_t *active_stream;

    pthread_mutex_t dgram_mtx;
    struct {
        void *data;
        size_t len;
        int pending;
    } out_dgram;
    struct {
        void *data;
        size_t len;
        int ready;
    } in_dgram;
#ifdef CWIST_WEBTRANSPORT
    cwist_webtransport_client_session *wt_connecting;
#endif
};

/* ------------------------------------------------------------------ */
/* Packet-out callback                                                */
/* ------------------------------------------------------------------ */

static int h3c_packets_out(void *ctx, const struct lsquic_out_spec *specs,
                           unsigned n_specs) {
    cwist_http3_client *client = ctx;
    unsigned i;
    for (i = 0; i < n_specs; ++i) {
        const struct lsquic_out_spec *spec = &specs[i];
        struct msghdr msg = {0};
        /* The socket is connected once the peer is known (see
         * cwist_http3_client_request); specifying a destination on a
         * connected UDP socket fails with EISCONN. */
        if (client->local_addr_len == 0) {
            msg.msg_name = (void *)spec->dest_sa;
            msg.msg_namelen = (spec->dest_sa && spec->dest_sa->sa_family == AF_INET)
                              ? sizeof(struct sockaddr_in)
                              : sizeof(struct sockaddr_in6);
        }
        msg.msg_iov = (struct iovec *)spec->iov;
        msg.msg_iovlen = spec->iovlen;
        ssize_t nw = sendmsg(client->udp_fd, &msg, MSG_DONTWAIT);
        if (nw < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return -1;
        }
    }
    return (int)i;
}

/* ------------------------------------------------------------------ */
/* Header-set interface (server sends us headers)                     */
/* ------------------------------------------------------------------ */

#define H3C_MAX_HEADERS 64
#define H3C_DECODE_BUF_SIZE 65536

typedef struct h3c_hset {
    struct lsxpack_header headers[H3C_MAX_HEADERS];
    size_t count;
    char decode_buf[H3C_DECODE_BUF_SIZE];
    size_t decode_off;
} h3c_hset_t;

static void *h3c_hsi_create(void *hsi_ctx, lsquic_stream_t *stream,
                            int is_push_promise) {
    (void)hsi_ctx;
    (void)stream;
    (void)is_push_promise;
    h3c_hset_t *hset = calloc(1, sizeof(*hset));
    return hset;
}

static struct lsxpack_header *
h3c_hsi_prepare(void *hset_p, struct lsxpack_header *xhdr, size_t req_space) {
    h3c_hset_t *hset = hset_p;
    if (!hset) return NULL;

    /* lsquic calls us again with the same header when its initial output
     * buffer was too small.  Preserve the decoded name and grow only the
     * value capacity; reinitializing the slot here loses the header. */
    if (xhdr) {
        if (req_space > LSXPACK_MAX_STRLEN || xhdr->name_offset < 0 ||
            (size_t)xhdr->name_offset >= sizeof(hset->decode_buf) ||
            req_space > sizeof(hset->decode_buf) - (size_t)xhdr->name_offset) {
            return NULL;
        }
        xhdr->val_len = (lsxpack_strlen_t)req_space;
        return xhdr;
    }

    if (hset->count >= H3C_MAX_HEADERS)
        return NULL;
    if (req_space > sizeof(hset->decode_buf) - hset->decode_off)
        return NULL;
    lsxpack_header_prepare_decode(&hset->headers[hset->count],
                                  hset->decode_buf, hset->decode_off,
                                  sizeof(hset->decode_buf) - hset->decode_off);
    return &hset->headers[hset->count];
}

static int h3c_hsi_process_header(void *hset_p, struct lsxpack_header *xhdr) {
    h3c_hset_t *hset = hset_p;
    /* A NULL header marks the end of a header block. */
    if (!hset || !xhdr)
        return 0;

    /* The QPACK decoder exposes the exact storage used by this completed
     * header. */
    size_t total = lsxpack_header_get_dec_size(xhdr);
    if (total > sizeof(hset->decode_buf) - hset->decode_off)
        return -1;
    hset->decode_off += total;
    hset->count++;
    return 0;
}

static void h3c_hsi_discard(void *hset_p) {
    free(hset_p);
}

static const struct lsquic_hset_if h3c_hset_if = {
    .hsi_create_header_set = h3c_hsi_create,
    .hsi_prepare_decode    = h3c_hsi_prepare,
    .hsi_process_header    = h3c_hsi_process_header,
    .hsi_discard_header_set= h3c_hsi_discard,
};

/* ------------------------------------------------------------------ */
/* Stream callbacks                                                   */
/* ------------------------------------------------------------------ */

static lsquic_conn_ctx_t *h3c_on_new_conn(void *stream_if_ctx,
                                           lsquic_conn_t *conn) {
    cwist_http3_client *client = stream_if_ctx;
    (void)conn;
    return (lsquic_conn_ctx_t *)client;
}

static void h3c_on_conn_closed(lsquic_conn_t *conn) {
    /* The engine destroys the connection after this callback; drop our
     * cached pointer so a later request never dereferences freed memory
     * (e.g. after a certificate verification failure). */
    cwist_http3_client *client =
        (cwist_http3_client *)lsquic_conn_get_ctx(conn);
    if (client && client->conn == conn) {
        client->conn = NULL;
    }
}

static lsquic_stream_ctx_t *h3c_on_new_stream(void *stream_if_ctx,
                                               lsquic_stream_t *stream) {
    cwist_http3_client *client = stream_if_ctx;
    h3c_stream_ctx_t *st = calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->stream = stream;
    st->req_method = CWIST_HTTP_GET;
    /* Do not wantwrite() here: the engine would dispatch h3c_on_write
     * before cwist_http3_client_request() has populated req_path/method/
     * headers/body, sending a default "GET /" instead of the real request.
     * The request function arms the write side once the fields are set. */
    pthread_mutex_lock(&client->mtx);
    client->active_stream = st;
    pthread_mutex_unlock(&client->mtx);
    return (lsquic_stream_ctx_t *)st;
}

#ifdef CWIST_WEBTRANSPORT
static lsquic_wt_session_ctx_t *
h3c_wt_on_session_open(void *ctx, lsquic_wt_session_t *native,
                       const struct lsquic_wt_connect_info *info) {
    (void)info;
    cwist_http3_client *client = ctx;
    if (!client || !client->wt_connecting) return NULL;
    pthread_mutex_lock(&client->mtx);
    client->wt_connecting->native = native;
    client->wt_connecting->open = 1;
    if (client->active_stream) client->active_stream->response_ready = 1;
    pthread_cond_broadcast(&client->cond);
    pthread_mutex_unlock(&client->mtx);
    return (lsquic_wt_session_ctx_t *)client->wt_connecting;
}

static void
h3c_wt_on_session_rejected(void *ctx, const struct lsquic_wt_connect_info *info,
                           unsigned status, const char *reason, size_t reason_len) {
    (void)info; (void)status; (void)reason; (void)reason_len;
    cwist_http3_client *client = ctx;
    if (!client || !client->wt_connecting) return;
    pthread_mutex_lock(&client->mtx);
    client->wt_connecting->rejected = 1;
    if (client->active_stream) client->active_stream->response_ready = 1;
    pthread_cond_broadcast(&client->cond);
    pthread_mutex_unlock(&client->mtx);
}

static void
h3c_wt_on_session_close(lsquic_wt_session_t *native, lsquic_wt_session_ctx_t *ctx,
                        uint64_t code, const char *reason, size_t reason_len) {
    (void)native; (void)code; (void)reason; (void)reason_len;
    cwist_webtransport_client_session *session =
        (cwist_webtransport_client_session *)ctx;
    if (session) {
        session->open = 0;
        session->native = NULL;
    }
}

static const struct lsquic_webtransport_if h3c_wt_if = {
    .wti_on_session_open = h3c_wt_on_session_open,
    .wti_on_session_rejected = h3c_wt_on_session_rejected,
    .wti_on_session_close = h3c_wt_on_session_close,
};
#endif

static void h3c_on_read(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    h3c_stream_ctx_t *st = (h3c_stream_ctx_t *)st_h;
    if (!st) return;

    unsigned char buf[8192];
    ssize_t nread;

    if (!st->headers_done) {
        void *hset = lsquic_stream_get_hset(stream);
        if (hset) {
            h3c_hset_t *hs = hset;
            st->res = cwist_http_response_create();
            size_t i;
            for (i = 0; i < hs->count && st->res; ++i) {
                const char *raw_name  = lsxpack_header_get_name(&hs->headers[i]);
                const char *raw_value = lsxpack_header_get_value(&hs->headers[i]);
                size_t name_len  = hs->headers[i].name_len;
                size_t value_len = hs->headers[i].val_len;
                if (!raw_name || !raw_value || name_len == 0 ||
                    name_len > 1024 || value_len > H3C_DECODE_BUF_SIZE - 1)
                    continue;
                /* lsxpack exposes counted slices, not C strings: copy before
                 * using strcmp/atoi, which read past the slice end. */
                char *name = malloc(name_len + 1);
                char *value = malloc(value_len + 1);
                if (!name || !value) {
                    free(name);
                    free(value);
                    break;
                }
                memcpy(name, raw_name, name_len);
                name[name_len] = '\0';
                memcpy(value, raw_value, value_len);
                value[value_len] = '\0';
                if (strcmp(name, ":status") == 0) {
                    st->res->status_code = (cwist_http_status_t)atoi(value);
                } else {
                    cwist_http_header_add(&st->res->headers, name, value);
                }
                free(value);
                free(name);
            }
            st->headers_done = 1;
#ifdef CWIST_WEBTRANSPORT
            if (st->is_webtransport_connect) {
                cwist_http3_client *client = (cwist_http3_client *)
                    lsquic_conn_get_ctx(lsquic_stream_conn(stream));
                if (!client || !st->res || st->res->status_code < 200 ||
                    st->res->status_code >= 300) {
                    if (client && client->wt_connecting)
                        client->wt_connecting->rejected = 1;
                    st->response_ready = 1;
                    return;
                }
                struct lsquic_wt_connect_info info = {
                    .wtci_authority = client->host,
                    .wtci_path = st->req_path,
                    .wtci_draft = lsquic_wt_peer_draft(lsquic_stream_conn(stream)),
                };
                struct lsquic_wt_accept_params params = {
                    .wtap_wt_if = &h3c_wt_if,
                    .wtap_wt_if_ctx = client,
                    .wtap_connect_info = &info,
                };
                if (lsquic_wt_accept(stream, &params) != 0) {
                    if (client->wt_connecting) client->wt_connecting->rejected = 1;
                    st->response_ready = 1;
                }
                return;
            }
#endif
        }
    }

    while ((nread = lsquic_stream_read(stream, buf, sizeof(buf))) > 0) {
        size_t need = st->resp_body_len + (size_t)nread;
        if (need > st->resp_body_cap) {
            size_t new_cap = st->resp_body_cap ? st->resp_body_cap * 2 : 4096;
            while (new_cap < need) new_cap *= 2;
            char *tmp = realloc(st->resp_body, new_cap);
            if (!tmp) {
                lsquic_stream_close(stream);
                return;
            }
            st->resp_body = tmp;
            st->resp_body_cap = new_cap;
        }
        memcpy(st->resp_body + st->resp_body_len, buf, (size_t)nread);
        st->resp_body_len += (size_t)nread;
    }

    if (nread == 0) {
        if (st->resp_body_len > 0 && st->res && st->res->body) {
            cwist_sstring_assign_len(st->res->body, st->resp_body, st->resp_body_len);
        }
        st->response_ready = 1;
        lsquic_stream_wantread(stream, 0);
    }
}

static void h3c_on_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    h3c_stream_ctx_t *st = (h3c_stream_ctx_t *)st_h;
    if (!st || st->write_done) return;

    /* Send request headers */
    struct lsxpack_header headers_arr[64];
    char hbuf[8192];
    size_t hbuf_off = 0;
    size_t hdr_count = 0;

    /* :method */
    const char *method_str = cwist_http_method_to_string(st->req_method);
    if (!method_str || !*method_str) method_str = "GET";
    size_t mlen = strlen(method_str);
    if (hbuf_off + 7 + 2 + mlen <= sizeof(hbuf)) {
        memcpy(hbuf + hbuf_off, ":method", 7);
        memcpy(hbuf + hbuf_off + 9, method_str, mlen);
        lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off,
                                   0, 7, 9, mlen);
        hbuf_off += 9 + mlen;
        hdr_count++;
    }

    /* :path */
    const char *path = st->req_path ? st->req_path : "/";
    size_t plen = strlen(path);
    if (hbuf_off + 5 + 2 + plen <= sizeof(hbuf)) {
        memcpy(hbuf + hbuf_off, ":path", 5);
        memcpy(hbuf + hbuf_off + 7, path, plen);
        lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off,
                                   0, 5, 7, plen);
        hbuf_off += 7 + plen;
        hdr_count++;
    }

    /* :authority */
    cwist_http3_client *client = (cwist_http3_client *)lsquic_conn_get_ctx(lsquic_stream_conn(stream));
    const char *authority = client && client->host ? client->host : "localhost";
    size_t alen = strlen(authority);
    if (hbuf_off + 10 + 2 + alen <= sizeof(hbuf)) {
        memcpy(hbuf + hbuf_off, ":authority", 10);
        memcpy(hbuf + hbuf_off + 12, authority, alen);
        lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off,
                                   0, 10, 12, alen);
        hbuf_off += 12 + alen;
        hdr_count++;
    }

    /* :scheme */
    const char *scheme = "https";
    size_t slen = strlen(scheme);
    if (hbuf_off + 7 + 2 + slen <= sizeof(hbuf)) {
        memcpy(hbuf + hbuf_off, ":scheme", 7);
        memcpy(hbuf + hbuf_off + 9, scheme, slen);
        lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off,
                                   0, 7, 9, slen);
        hbuf_off += 9 + slen;
        hdr_count++;
    }

    cwist_http_header_node *node = st->req_headers;
    while (node && hdr_count < 64) {
        if (!node->key || !node->key->data || !node->value || !node->value->data) {
            node = node->next;
            continue;
        }
        const char *name = node->key->data;
        const char *value = node->value->data;
        size_t nlen = node->key->size;
        size_t vlen = node->value->size;
        if ((nlen == 7 && strncasecmp(name, ":method", nlen) == 0) ||
            (nlen == 5 && strncasecmp(name, ":path", nlen) == 0) ||
            (nlen == 10 && strncasecmp(name, ":authority", nlen) == 0) ||
            (nlen == 7 && strncasecmp(name, ":scheme", nlen) == 0)) {
            node = node->next;
            continue;
        }
        if (hbuf_off + nlen + 2 + vlen <= sizeof(hbuf)) {
            memcpy(hbuf + hbuf_off, name, nlen);
            memcpy(hbuf + hbuf_off + nlen + 2, value, vlen);
            lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off,
                                       0, nlen, nlen + 2, vlen);
            hbuf_off += nlen + 2 + vlen;
            hdr_count++;
        }
        node = node->next;
    }

    lsquic_http_headers_t headers = {
        .count = (unsigned)hdr_count,
        .headers = headers_arr,
    };

    int has_body = (st->req_body && st->req_body_len > 0);
    if (lsquic_stream_send_headers(stream, &headers, !has_body) != 0) {
        lsquic_stream_close(stream);
        return;
    }

    if (has_body) {
        ssize_t n = lsquic_stream_write(stream, st->req_body + st->req_body_sent,
                                        st->req_body_len - st->req_body_sent);
        if (n < 0) {
            lsquic_stream_close(stream);
            return;
        }
        st->req_body_sent += (size_t)n;
        if (st->req_body_sent >= st->req_body_len) {
            lsquic_stream_shutdown(stream, 1);
            st->write_done = 1;
            lsquic_stream_wantwrite(stream, 0);
            lsquic_stream_wantread(stream, 1);
        }
    } else {
        st->write_done = 1;
        /* lsquic ignores the eos argument of lsquic_stream_send_headers()
         * for IETF QUIC; shut down the write side explicitly so the server
         * sees FIN on a bodyless request. */
        lsquic_stream_shutdown(stream, 1);
        lsquic_stream_wantwrite(stream, 0);
        lsquic_stream_wantread(stream, 1);
    }
}

static void h3c_on_close(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    h3c_stream_ctx_t *st = (h3c_stream_ctx_t *)st_h;
    if (!st) return;
    cwist_http3_client *client = (cwist_http3_client *)lsquic_conn_get_ctx(lsquic_stream_conn(stream));
    pthread_mutex_lock(&client->mtx);
    st->response_ready = 1;
    if (client->active_stream == st) {
        client->active_stream = NULL;
    }
    pthread_cond_broadcast(&client->cond);
    pthread_mutex_unlock(&client->mtx);
    free(st->req_path);
    free(st->req_body);
    free(st->resp_body);
    free(st);
}

static ssize_t h3c_on_dg_write(lsquic_conn_t *conn, void *buf, size_t len) {
    cwist_http3_client *client = (cwist_http3_client *)lsquic_conn_get_ctx(conn);
    if (!client) return 0;
    pthread_mutex_lock(&client->dgram_mtx);
    if (client->out_dgram.pending && client->out_dgram.data && client->out_dgram.len > 0) {
        size_t to_copy = client->out_dgram.len < len ? client->out_dgram.len : len;
        memcpy(buf, client->out_dgram.data, to_copy);
        free(client->out_dgram.data);
        client->out_dgram.data = NULL;
        client->out_dgram.len = 0;
        client->out_dgram.pending = 0;
        pthread_mutex_unlock(&client->dgram_mtx);
        return (ssize_t)to_copy;
    }
    pthread_mutex_unlock(&client->dgram_mtx);
    return 0;
}

static void h3c_on_datagram(lsquic_conn_t *conn, const void *buf, size_t len) {
    cwist_http3_client *client = (cwist_http3_client *)lsquic_conn_get_ctx(conn);
    if (!client || !buf || len == 0) return;
    pthread_mutex_lock(&client->dgram_mtx);
    if (client->in_dgram.data) free(client->in_dgram.data);
    client->in_dgram.data = malloc(len);
    if (client->in_dgram.data) {
        memcpy(client->in_dgram.data, buf, len);
        client->in_dgram.len = len;
        client->in_dgram.ready = 1;
    }
    pthread_mutex_unlock(&client->dgram_mtx);
}

static const struct lsquic_stream_if h3c_stream_if = {
    .on_new_conn    = h3c_on_new_conn,
    .on_conn_closed = h3c_on_conn_closed,
    .on_new_stream  = h3c_on_new_stream,
    .on_read        = h3c_on_read,
    .on_write       = h3c_on_write,
    .on_close       = h3c_on_close,
    .on_dg_write    = h3c_on_dg_write,
    .on_datagram    = h3c_on_datagram,
};

/* ------------------------------------------------------------------ */
/* I/O loop helper                                                    */
/* ------------------------------------------------------------------ */

static void h3c_process_io(cwist_http3_client *client, int timeout_ms) {
    lsquic_engine_t *engine = client->engine;
    int diff = timeout_ms * 1000; /* microseconds */
    if (diff <= 0) diff = 1000;

    if (lsquic_engine_earliest_adv_tick(engine, &diff)) {
        if (diff <= 0) diff = 0;
        else if (diff > 1000000) diff = 1000000;
    }

    struct pollfd pfd = { .fd = client->udp_fd, .events = POLLIN };
    int pret = poll(&pfd, 1, diff / 1000);

    if (pret > 0 && (pfd.revents & POLLIN)) {
        unsigned char buf[65535];
        struct sockaddr_storage peer_addr;
        socklen_t peer_addr_len = sizeof(peer_addr);
        ssize_t nr = recvfrom(client->udp_fd, buf, sizeof(buf), 0,
                              (struct sockaddr *)&peer_addr, &peer_addr_len);
        if (nr > 0) {
            lsquic_engine_packet_in(engine, buf, (size_t)nr,
                                    client->local_addr_len
                                        ? (struct sockaddr *)&client->local_addr
                                        : NULL,
                                    (struct sockaddr *)&peer_addr,
                                    NULL, 0);
        }
    }

    lsquic_engine_process_conns(engine);
}

/* ------------------------------------------------------------------ */
/* SSL context callback: hand lsquic the client SSL_CTX so the        */
/* handshake honors its trust store and verify mode.                  */
/* ------------------------------------------------------------------ */

static SSL_CTX *h3c_get_ssl_ctx(void *peer_ctx, const struct sockaddr *local) {
    (void)local;
    cwist_http3_client *client = peer_ctx;
    return client ? client->ssl_ctx : NULL;
}

/* ------------------------------------------------------------------ */
/* Client API                                                         */
/* ------------------------------------------------------------------ */

cwist_http3_client *cwist_http3_client_create(void) {
    h3c_global_init();

    cwist_http3_client *client = calloc(1, sizeof(*client));
    if (!client) {
        h3c_global_cleanup();
        return NULL;
    }

    client->udp_fd = -1;
    client->timeout_ms = 30000;
    client->max_retries = 0;
    client->retry_delay_ms = 1000;
    client->conn_timeout_ms = 5000;
    pthread_mutex_init(&client->mtx, NULL);
    pthread_cond_init(&client->cond, NULL);
    pthread_mutex_init(&client->dgram_mtx, NULL);

    const SSL_METHOD *method = TLS_method();
    client->ssl_ctx = SSL_CTX_new(method);
    if (!client->ssl_ctx) {
        free(client);
        h3c_global_cleanup();
        return NULL;
    }

    SSL_CTX_set_min_proto_version(client->ssl_ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(client->ssl_ctx, TLS1_3_VERSION);
    /* Secure default: verify the peer certificate chain and hostname
     * (SNI set by lsquic also feeds BoringSSL's X509_VERIFY_PARAM host
     * check).  cwist_http3_client_set_insecure() opts out explicitly. */
    SSL_CTX_set_verify(client->ssl_ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_default_verify_paths(client->ssl_ctx);

    struct lsquic_engine_settings settings;
    lsquic_engine_init_settings(&settings, LSENG_HTTP);
    settings.es_versions = (1 << LSQVER_I001) | (1 << LSQVER_I002);
    settings.es_ping_period = 15;
    settings.es_noprogress_timeout = 30;
    settings.es_ecn = 1;
    settings.es_pace_packets = 1;
    settings.es_optimistic_nat = 1;
    settings.es_datagrams = client->datagram_enabled ? 1 : 0;
#ifdef CWIST_WEBTRANSPORT
    settings.es_webtransport = 1;
    settings.es_max_webtransport_sessions = 4;
    settings.es_http_datagrams = 1;
    settings.es_reset_stream_at = 1;
#endif

    char err_buf[256];
    if (lsquic_engine_check_settings(&settings, LSENG_HTTP,
                                     err_buf, sizeof(err_buf)) != 0) {
        fprintf(stderr, "[HTTP/3-CLIENT] Invalid engine settings: %s\n", err_buf);
        SSL_CTX_free(client->ssl_ctx);
        pthread_mutex_destroy(&client->mtx);
        pthread_cond_destroy(&client->cond);
        free(client);
        h3c_global_cleanup();
        return NULL;
    }

    struct lsquic_engine_api api = {
        .ea_stream_if        = &h3c_stream_if,
        .ea_stream_if_ctx    = client,
        .ea_packets_out      = h3c_packets_out,
        .ea_packets_out_ctx  = client,
        .ea_get_ssl_ctx      = h3c_get_ssl_ctx,
        .ea_hsi_if           = &h3c_hset_if,
        .ea_hsi_ctx          = NULL,
        .ea_settings         = &settings,
        .ea_alpn             = "h3",
    };

    client->engine = lsquic_engine_new(LSENG_HTTP, &api);
    if (!client->engine) {
        SSL_CTX_free(client->ssl_ctx);
        pthread_mutex_destroy(&client->mtx);
        pthread_cond_destroy(&client->cond);
        free(client);
        h3c_global_cleanup();
        return NULL;
    }

    return client;
}

void cwist_http3_client_destroy(cwist_http3_client *client) {
    if (!client) return;
    if (client->engine) {
        lsquic_engine_destroy(client->engine);
    }
    if (client->ssl_ctx) {
        SSL_CTX_free(client->ssl_ctx);
    }
    if (client->udp_fd >= 0) {
        close(client->udp_fd);
    }
    free(client->host);
    free(client->out_dgram.data);
    free(client->in_dgram.data);
#ifdef CWIST_WEBTRANSPORT
    free(client->wt_connecting);
#endif
    pthread_mutex_destroy(&client->dgram_mtx);
    pthread_mutex_destroy(&client->mtx);
    pthread_cond_destroy(&client->cond);
    free(client);
    h3c_global_cleanup();
}

int cwist_http3_client_set_server(cwist_http3_client *client,
                                  const char *host, uint16_t port) {
    if (!client || !host) return -1;
    free(client->host);
    client->host = strdup(host);
    client->port = port;
    return 0;
}

int cwist_http3_client_set_ca_bundle(cwist_http3_client *client,
                                     const char *ca_path) {
    if (!client || !client->ssl_ctx) return -1;
    /* Verification stays on either way; this only selects the trust store. */
    if (ca_path) {
        if (SSL_CTX_load_verify_locations(client->ssl_ctx, ca_path, NULL) != 1)
            return -1;
    } else {
        SSL_CTX_set_default_verify_paths(client->ssl_ctx);
    }
    return 0;
}

void cwist_http3_client_set_insecure(cwist_http3_client *client, int enabled) {
    if (!client || !client->ssl_ctx) return;
    SSL_CTX_set_verify(client->ssl_ctx,
                       enabled ? SSL_VERIFY_NONE : SSL_VERIFY_PEER, NULL);
}

void cwist_http3_client_set_timeout_ms(cwist_http3_client *client,
                                       int timeout_ms) {
    if (client) client->timeout_ms = timeout_ms;
}

void cwist_http3_client_enable_0rtt(cwist_http3_client *client, int enabled) {
    if (!client || !client->ssl_ctx) return;
    SSL_CTX_set_early_data_enabled(client->ssl_ctx, enabled ? 1 : 0);
}

void cwist_http3_client_enable_datagrams(cwist_http3_client *client,
                                         int enabled) {
    if (client) client->datagram_enabled = enabled;
}

/* ------------------------------------------------------------------ */
/* Synchronous request                                                */
/* ------------------------------------------------------------------ */

cwist_error_t cwist_http3_client_request(cwist_http3_client *client,
                                         const char *path,
                                         cwist_http_method_t method,
                                         cwist_http_header_node *headers,
                                         const char *body,
                                         size_t body_len,
                                         cwist_http_response **out_response) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!client || !client->engine || !client->host || !path || !out_response) {
        err.error.err_i16 = -1;
        return err;
    }

    *out_response = NULL;

    /* Create UDP socket if needed */
    if (client->udp_fd < 0) {
        client->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (client->udp_fd < 0) {
            err.error.err_i16 = -1;
            return err;
        }
        int flags = fcntl(client->udp_fd, F_GETFL, 0);
        if (flags >= 0) fcntl(client->udp_fd, F_SETFL, flags | O_NONBLOCK);
    }

    /* Resolve host if not already done */
    if (client->peer_addr_len == 0) {
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%u", client->port);
        if (getaddrinfo(client->host, port_str, &hints, &res) != 0 || !res) {
            err.error.err_i16 = -1;
            return err;
        }
        memcpy(&client->peer_addr, res->ai_addr, res->ai_addrlen);
        client->peer_addr_len = res->ai_addrlen;
        freeaddrinfo(res);
    }

    /* lsquic dereferences the local address in client mode
     * (ietf_full_conn_ci_record_addrs), so a NULL local_sa segfaults.
     * Connect the UDP socket to the peer and learn the local endpoint. */
    if (client->local_addr_len == 0) {
        if (connect(client->udp_fd, (struct sockaddr *)&client->peer_addr,
                    client->peer_addr_len) != 0) {
            err.error.err_i16 = -1;
            return err;
        }
        client->local_addr_len = sizeof(client->local_addr);
        if (getsockname(client->udp_fd, (struct sockaddr *)&client->local_addr,
                        &client->local_addr_len) != 0) {
            client->local_addr_len = 0;
            err.error.err_i16 = -1;
            return err;
        }
    }

    /* ---------------------------------------------------------------- */
    /* Retry loop with exponential backoff                               */
    /* ---------------------------------------------------------------- */
    int attempt = 0;
    int max_attempts = 1 + client->max_retries;
    while (attempt < max_attempts) {
        /* Ensure connection is healthy */
        if (client->conn) {
            enum LSQUIC_CONN_STATUS st_status = lsquic_conn_status(client->conn, NULL, 0);
            if (st_status == LSCONN_ST_CLOSED || st_status == LSCONN_ST_TIMED_OUT ||
                st_status == LSCONN_ST_ERROR || st_status == LSCONN_ST_RESET ||
                st_status == LSCONN_ST_HSK_FAILURE) {
                client->conn = NULL; /* lsquic will clean it up via engine */
            }
        }

        if (!client->conn) {
            client->conn = lsquic_engine_connect(client->engine, N_LSQVER,
                                                  (struct sockaddr *)&client->local_addr,
                                                  (struct sockaddr *)&client->peer_addr,
                                                  client, NULL,
                                                  client->host, 0,
                                                  NULL, 0, NULL, 0);
            if (!client->conn) {
                err.error.err_i16 = -1;
                goto retry_backoff;
            }
        }

        /* Request a new stream */
        pthread_mutex_lock(&client->mtx);
        client->active_stream = NULL;
        pthread_mutex_unlock(&client->mtx);
        lsquic_conn_make_stream(client->conn);

        /* I/O loop until stream is created */
        struct timespec stream_deadline;
        clock_gettime(CLOCK_REALTIME, &stream_deadline);
        stream_deadline.tv_sec += client->conn_timeout_ms / 1000;
        stream_deadline.tv_nsec += (client->conn_timeout_ms % 1000) * 1000000;
        if (stream_deadline.tv_nsec >= 1000000000) {
            stream_deadline.tv_sec++;
            stream_deadline.tv_nsec -= 1000000000;
        }
        h3c_stream_ctx_t *st = NULL;
        while (!st) {
            h3c_process_io(client, 100);
            pthread_mutex_lock(&client->mtx);
            st = client->active_stream;
            pthread_mutex_unlock(&client->mtx);
            if (st) break;
            /* Fail fast when the handshake is dead (e.g. certificate
             * verification failure) instead of waiting out the timeout.
             * h3c_on_conn_closed() NULLs client->conn once the engine
             * tears the connection down. */
            if (!client->conn) break;
            enum LSQUIC_CONN_STATUS cst =
                lsquic_conn_status(client->conn, NULL, 0);
            if (cst == LSCONN_ST_HSK_FAILURE || cst == LSCONN_ST_ERROR ||
                cst == LSCONN_ST_CLOSED || cst == LSCONN_ST_TIMED_OUT ||
                cst == LSCONN_ST_RESET) {
                /* The engine owns and may already have destroyed the failed
                 * connection; drop our pointer before the next attempt. */
                client->conn = NULL;
                break;
            }
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > stream_deadline.tv_sec ||
                (now.tv_sec == stream_deadline.tv_sec && now.tv_nsec >= stream_deadline.tv_nsec)) {
                break;
            }
        }
        if (!st) {
            err.error.err_i16 = -1;
            goto retry_backoff;
        }

        st->req_path = strdup(path);
        if (!st->req_path) {
            err.error.err_i16 = -1;
            goto retry_backoff;
        }
        st->req_method = method;
        st->req_headers = headers;
#ifdef CWIST_WEBTRANSPORT
        for (cwist_http_header_node *header = headers; header; header = header->next) {
            if (header->key && header->value && header->key->data && header->value->data &&
                strcasecmp(header->key->data, ":protocol") == 0 &&
                strcasecmp(header->value->data, "webtransport") == 0) {
                st->is_webtransport_connect = 1;
                break;
            }
        }
#endif

        /* Store request body */
        if (body && body_len > 0) {
            st->req_body = malloc(body_len);
            if (!st->req_body) {
                err.error.err_i16 = -1;
                goto retry_backoff;
            }
            memcpy(st->req_body, body, body_len);
            st->req_body_len = body_len;
        }

        /* All request fields are in place: arm the write side now. */
        lsquic_stream_wantwrite(st->stream, 1);

        /* I/O loop until response is ready or timeout */
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += client->timeout_ms / 1000;
        deadline.tv_nsec += (client->timeout_ms % 1000) * 1000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }

        pthread_mutex_lock(&client->mtx);
        while (!st->response_ready) {
            pthread_mutex_unlock(&client->mtx);
            h3c_process_io(client, 100);
            pthread_mutex_lock(&client->mtx);
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
                break;
            }
        }
        pthread_mutex_unlock(&client->mtx);

        if (st->response_ready) {
            *out_response = st->res;
            st->res = NULL; /* Transfer ownership */
            err.error.err_i16 = 0;
            return err;
        }

        /* Timeout on this attempt: close the stream so lsquic releases the
         * context via h3c_on_close.  Freeing it here would leave lsquic
         * holding a dangling stream ctx (use-after-free on the next event). */
        err.error.err_i16 = -1;
        pthread_mutex_lock(&client->mtx);
        if (client->active_stream == st) {
            client->active_stream = NULL;
        }
        pthread_mutex_unlock(&client->mtx);
        if (st->res) {
            cwist_http_response_destroy(st->res);
            st->res = NULL;
        }
        lsquic_stream_close(st->stream);

    retry_backoff:
        attempt++;
        if (attempt < max_attempts && client->retry_delay_ms > 0) {
            int backoff = client->retry_delay_ms * (1 << (attempt - 1));
            if (backoff > 30000) backoff = 30000; /* cap at 30s */
            usleep((useconds_t)backoff * 1000);
        }
    }

    return err;
}

/* ------------------------------------------------------------------ */
/* Datagram API (RFC 9221)                                            */
/* ------------------------------------------------------------------ */

int cwist_http3_client_send_datagram(cwist_http3_client *client,
                                     const void *data, size_t len) {
    if (!client || !client->conn || !data || len == 0) return -1;
    if (!client->datagram_enabled) return -1;

    pthread_mutex_lock(&client->dgram_mtx);
    if (client->out_dgram.pending) {
        pthread_mutex_unlock(&client->dgram_mtx);
        return -1;
    }
    client->out_dgram.data = malloc(len);
    if (!client->out_dgram.data) {
        pthread_mutex_unlock(&client->dgram_mtx);
        return -1;
    }
    memcpy(client->out_dgram.data, data, len);
    client->out_dgram.len = len;
    client->out_dgram.pending = 1;
    pthread_mutex_unlock(&client->dgram_mtx);

    lsquic_conn_want_datagram_write(client->conn, 1);
    return 0;
}

ssize_t cwist_http3_client_recv_datagram(cwist_http3_client *client,
                                         void *buf, size_t len) {
    if (!client || !buf || len == 0) return -1;

    pthread_mutex_lock(&client->dgram_mtx);
    if (!client->in_dgram.ready || !client->in_dgram.data) {
        pthread_mutex_unlock(&client->dgram_mtx);
        return -1;
    }
    size_t to_copy = client->in_dgram.len < len ? client->in_dgram.len : len;
    memcpy(buf, client->in_dgram.data, to_copy);
    free(client->in_dgram.data);
    client->in_dgram.data = NULL;
    client->in_dgram.len = 0;
    client->in_dgram.ready = 0;
    pthread_mutex_unlock(&client->dgram_mtx);
    return (ssize_t)to_copy;
}

/* ------------------------------------------------------------------ */
/* WebTransport client (LSQUIC proposal API)                          */
/* ------------------------------------------------------------------ */

cwist_error_t
cwist_http3_client_webtransport_connect(cwist_http3_client *client,
                                         const char *path, const char *origin,
                                         cwist_webtransport_client_session **out_session) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;
    if (!client || !path || !out_session) return err;
    *out_session = NULL;
#ifndef CWIST_WEBTRANSPORT
    (void)origin;
    return err;
#else
    if (client->wt_connecting && client->wt_connecting->open) return err;

    cwist_webtransport_client_session *session = calloc(1, sizeof(*session));
    if (!session) return err;
    session->client = client;
    client->wt_connecting = session;

    cwist_http_header_node *headers = NULL;
    if (cwist_http_header_add(&headers, ":protocol", "webtransport").error.err_i16 != 0 ||
        (origin && cwist_http_header_add(&headers, "origin", origin).error.err_i16 != 0)) {
        cwist_http_header_free_all(headers);
        free(session);
        client->wt_connecting = NULL;
        return err;
    }

    cwist_http_response *response = NULL;
    err = cwist_http3_client_request(client, path, CWIST_HTTP_CONNECT,
                                     headers, NULL, 0, &response);
    cwist_http_header_free_all(headers);
    if (response) cwist_http_response_destroy(response);
    if (err.error.err_i16 != 0 || !session->open || !session->native) {
        if (client->wt_connecting == session) client->wt_connecting = NULL;
        free(session);
        err.error.err_i16 = -1;
        return err;
    }
    *out_session = session;
    return err;
#endif
}

int cwist_webtransport_client_poll(cwist_http3_client *client, int timeout_ms) {
    if (!client || !client->engine || client->udp_fd < 0) return -1;
    h3c_process_io(client, timeout_ms > 0 ? timeout_ms : 1);
    return 0;
}

int cwist_webtransport_client_is_open(const cwist_webtransport_client_session *session) {
#ifdef CWIST_WEBTRANSPORT
    return session && session->open && session->native;
#else
    (void)session;
    return 0;
#endif
}

void *cwist_webtransport_client_open_bidi(cwist_webtransport_client_session *session) {
#ifdef CWIST_WEBTRANSPORT
    return cwist_webtransport_client_is_open(session)
        ? lsquic_wt_open_bidi(session->native) : NULL;
#else
    (void)session;
    return NULL;
#endif
}

void *cwist_webtransport_client_open_uni(cwist_webtransport_client_session *session) {
#ifdef CWIST_WEBTRANSPORT
    return cwist_webtransport_client_is_open(session)
        ? lsquic_wt_open_uni(session->native) : NULL;
#else
    (void)session;
    return NULL;
#endif
}

ssize_t cwist_webtransport_client_stream_read(void *stream, void *buf, size_t len) {
    return stream && buf && len ? lsquic_stream_read((lsquic_stream_t *)stream, buf, len) : -1;
}

ssize_t cwist_webtransport_client_stream_write(void *stream, const void *buf, size_t len) {
    return stream && buf && len ? lsquic_stream_write((lsquic_stream_t *)stream, buf, len) : -1;
}

int cwist_webtransport_client_stream_flush(void *stream) {
    return stream ? lsquic_stream_flush((lsquic_stream_t *)stream) : -1;
}

int cwist_webtransport_client_stream_close(void *stream) {
    if (!stream) return -1;
    lsquic_stream_close((lsquic_stream_t *)stream);
    return 0;
}

ssize_t cwist_webtransport_client_send_datagram(cwist_webtransport_client_session *session,
                                                const void *data, size_t len) {
#ifdef CWIST_WEBTRANSPORT
    return cwist_webtransport_client_is_open(session) && data && len
        ? lsquic_wt_send_datagram(session->native, data, len) : -1;
#else
    (void)session; (void)data; (void)len;
    return -1;
#endif
}

int cwist_webtransport_client_close(cwist_webtransport_client_session *session,
                                    uint64_t code, const char *reason) {
#ifdef CWIST_WEBTRANSPORT
    if (!cwist_webtransport_client_is_open(session)) return -1;
    return lsquic_wt_close(session->native, code, reason,
                           reason ? strlen(reason) : 0);
#else
    (void)session; (void)code; (void)reason;
    return -1;
#endif
}

/* ------------------------------------------------------------------ */
/* Resilience knobs                                                   */
/* ------------------------------------------------------------------ */

void cwist_http3_client_set_max_retries(cwist_http3_client *client,
                                        int max_retries) {
    if (client) client->max_retries = max_retries > 0 ? max_retries : 0;
}

void cwist_http3_client_set_retry_delay_ms(cwist_http3_client *client,
                                           int delay_ms) {
    if (client) client->retry_delay_ms = delay_ms > 0 ? delay_ms : 0;
}

void cwist_http3_client_set_conn_timeout_ms(cwist_http3_client *client,
                                            int timeout_ms) {
    if (client) client->conn_timeout_ms = timeout_ms > 0 ? timeout_ms : 5000;
}
