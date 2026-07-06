/**
 * @file http3_client.c
 * @brief lsquic/BoringSSL-based HTTP/3 client for CWIST.
 */

#define _POSIX_C_SOURCE 200809L
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
#include <lsxpack_header.h>

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
} h3c_stream_ctx_t;

struct cwist_http3_client {
    SSL_CTX *ssl_ctx;
    lsquic_engine_t *engine;
    lsquic_conn_t *conn;
    int udp_fd;
    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len;
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
        msg.msg_name = (void *)spec->dest_sa;
        msg.msg_namelen = (spec->dest_sa && spec->dest_sa->sa_family == AF_INET)
                          ? sizeof(struct sockaddr_in)
                          : sizeof(struct sockaddr_in6);
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
    if (xhdr) {
        /* Advance by the exact decoded size lsquic reports
         * (name_len + val_len + dec_overhead). */
        size_t total = lsxpack_header_get_dec_size(xhdr);
        if (total > sizeof(hset->decode_buf) - hset->decode_off)
            total = sizeof(hset->decode_buf) - hset->decode_off;
        hset->decode_off += total;
        if (hset->count < H3C_MAX_HEADERS)
            hset->count++;
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
    (void)hset_p;
    (void)xhdr;
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
    (void)conn;
}

static lsquic_stream_ctx_t *h3c_on_new_stream(void *stream_if_ctx,
                                               lsquic_stream_t *stream) {
    cwist_http3_client *client = stream_if_ctx;
    h3c_stream_ctx_t *st = calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->stream = stream;
    st->req_method = CWIST_HTTP_GET;
    lsquic_stream_wantwrite(stream, 1);
    pthread_mutex_lock(&client->mtx);
    client->active_stream = st;
    pthread_mutex_unlock(&client->mtx);
    return (lsquic_stream_ctx_t *)st;
}

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
            for (i = 0; i < hs->count; ++i) {
                const char *name  = lsxpack_header_get_name(&hs->headers[i]);
                const char *value = lsxpack_header_get_value(&hs->headers[i]);
                if (!name || !value) continue;
                if (strcmp(name, ":status") == 0) {
                    st->res->status_code = (cwist_http_status_t)atoi(value);
                } else {
                    cwist_http_header_add(&st->res->headers, name, value);
                }
            }
            st->headers_done = 1;
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

static const struct lsquic_stream_if h3c_stream_if = {
    .on_new_conn    = h3c_on_new_conn,
    .on_conn_closed = h3c_on_conn_closed,
    .on_new_stream  = h3c_on_new_stream,
    .on_read        = h3c_on_read,
    .on_write       = h3c_on_write,
    .on_close       = h3c_on_close,
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
                                    NULL, (struct sockaddr *)&peer_addr,
                                    NULL, 0);
        }
    }

    lsquic_engine_process_conns(engine);
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

    const SSL_METHOD *method = TLS_method();
    client->ssl_ctx = SSL_CTX_new(method);
    if (!client->ssl_ctx) {
        free(client);
        h3c_global_cleanup();
        return NULL;
    }

    SSL_CTX_set_min_proto_version(client->ssl_ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(client->ssl_ctx, TLS1_3_VERSION);
    SSL_CTX_set_default_verify_paths(client->ssl_ctx);

    struct lsquic_engine_settings settings;
    lsquic_engine_init_settings(&settings, LSENG_HTTP);
    settings.es_versions = (1 << LSQVER_I001) | (1 << LSQVER_I002);
    settings.es_ping_period = 15;
    settings.es_noprogress_timeout = 30;
    settings.es_ecn = 1;
    settings.es_pace_packets = 1;
    settings.es_optimistic_nat = 1;

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
    if (ca_path) {
        if (SSL_CTX_load_verify_locations(client->ssl_ctx, ca_path, NULL) != 1)
            return -1;
    } else {
        SSL_CTX_set_verify(client->ssl_ctx, SSL_VERIFY_NONE, NULL);
    }
    return 0;
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
                                                  NULL,
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

        /* Timeout on this attempt – clean up stream state and retry */
        err.error.err_i16 = -1;
        pthread_mutex_lock(&client->mtx);
        client->active_stream = NULL;
        pthread_mutex_unlock(&client->mtx);
        free(st->req_body);
        free(st->req_path);
        st->req_body = NULL;
        st->req_path = NULL;
        free(st);

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
/* Datagram stubs (RFC 9221)                                          */
/* ------------------------------------------------------------------ */

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

int cwist_http3_client_send_datagram(cwist_http3_client *client,
                                     const void *data, size_t len) {
    (void)client;
    (void)data;
    (void)len;
    return -1; /* Not yet implemented: requires lsquic datagram API */
}

ssize_t cwist_http3_client_recv_datagram(cwist_http3_client *client,
                                         void *buf, size_t len) {
    (void)client;
    (void)buf;
    (void)len;
    return -1; /* Not yet implemented: requires lsquic datagram API */
}
