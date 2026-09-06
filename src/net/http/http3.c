/**
 * @file http3.c
 * @brief lsquic/BoringSSL-based HTTP/3 server for CWIST.
 *
 * Implements a full HTTP/3 server using LiteSpeed's lsquic library
 * linked statically against BoringSSL.  Handles QUIC transport,
 * QPACK, and HTTP/3 framing per RFC 9114.
 */

#if defined(__APPLE__)
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#elif !defined(__FreeBSD__) && !defined(__NetBSD__) && !defined(__OpenBSD__) && !defined(__DragonFly__)
#define _POSIX_C_SOURCE 200809L
#endif
#include <cwist/net/http/http3.h>
#include <cwist/net/http/http2.h>
#include <cwist/core/log.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/seq/seq.h>
#include <cwist/sys/err/cwist_err.h>
#include <cwist/sys/app/shutdown.h>
#include <ttak/timing/timing.h>
#include <ttak/async/sched.h>
#include "tls_chain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
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
#include <openssl/rand.h>
#include <openssl/hmac.h>

#include <lsquic.h>
#ifdef CWIST_WEBTRANSPORT
#include <lsquic_wt.h>
#endif
#include <lsxpack_header.h>

/* BSD sockets do not universally provide MSG_DONTWAIT.  The UDP socket is
 * configured non-blocking before lsquic can emit packets, so no flag is
 * needed on platforms that omit it. */
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

/* ECN ancillary data is optional across supported socket implementations. */
#if defined(CMSG_SPACE) && defined(CMSG_FIRSTHDR) && defined(CMSG_NXTHDR) && defined(IP_TOS)
#define CWIST_H3_HAVE_ECN_CMSG 1
#endif

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

/* RFC 9114 Section 4.3.1 request pseudo-header tracking (bitmask) */
#define H3_PSEUDO_METHOD    0x01u
#define H3_PSEUDO_SCHEME    0x02u
#define H3_PSEUDO_PATH      0x04u
#define H3_PSEUDO_AUTHORITY 0x08u
#define H3_PSEUDO_PROTOCOL  0x10u

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
    int sequenced_data; /* X-CWIST-Sequenced-Data: 1 request body */
    unsigned char *seq_buf;
    size_t seq_len;
    size_t seq_cap;
    cwist_seq_assembler_t *body_assembler;
    unsigned pseudo_seen;    /* H3_PSEUDO_* bits seen so far */
    int seen_regular_header; /* a non-pseudo header already arrived */
    int is_connect;          /* :method CONNECT */
    int saw_empty_path;      /* :path arrived with a zero-length value */
    int malformed;           /* RFC 9114 Section 4.3.1 violation */
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

/* Browsers routinely send more than 64 HTTP/3 headers once Client Hints,
 * security metadata and cookies are included.  Keep a firm per-stream cap,
 * but leave enough headroom that a late Cookie or :path is never discarded. */
#define H3_MAX_HEADERS 256
#define H3_DECODE_BUF_SIZE 131072
#define H3_MAX_RESPONSE_HEADERS 256
#define H3_RESPONSE_HEADER_BUF_SIZE 32768

typedef struct cwist_h3_hset {
    lsquic_stream_t *stream;
    struct cwist_h3_hset *next;  /* intrusive list of live hsets per ctx */
    struct cwist_h3_hset **prev; /* link to the pointer that points at us */
    cwist_http3_context *owner;  /* tracking context, NULL if untracked */
    struct lsxpack_header headers[H3_MAX_HEADERS];
    size_t count;
    char decode_buf[H3_DECODE_BUF_SIZE];
    size_t decode_off;
} cwist_h3_hset_t;

/* The engine loop is single-threaded and all hsi callbacks fire from it (or
 * from engine destroy after the loop has exited), so the list needs no
 * locking. */
static void cwist_h3_hset_track(cwist_http3_context *ctx, cwist_h3_hset_t *hset) {
    if (!ctx || !hset) return;
    hset->owner = ctx;
    hset->next = (cwist_h3_hset_t *)ctx->hsets;
    hset->prev = (cwist_h3_hset_t **)&ctx->hsets;
    if (hset->next) hset->next->prev = &hset->next;
    ctx->hsets = hset;
}

static void cwist_h3_hset_untrack(cwist_h3_hset_t *hset) {
    if (!hset || !hset->owner) return;
    *hset->prev = hset->next;
    if (hset->next) hset->next->prev = hset->prev;
    hset->next = NULL;
    hset->prev = NULL;
    hset->owner = NULL;
}

/* lsquic never calls hsi_discard_header_set for streams that are still open
 * when the engine is destroyed; free whatever is still tracked.  Must run
 * after lsquic_engine_destroy() so a discard issued during destroy has
 * already untracked its hset (no double-free). */
static void cwist_h3_hset_sweep(cwist_http3_context *ctx) {
    if (!ctx) return;
    while (ctx->hsets) {
        cwist_h3_hset_t *hset = (cwist_h3_hset_t *)ctx->hsets;
        cwist_h3_hset_untrack(hset);
        free(hset);
    }
}

static void *cwist_h3_hsi_create(void *hsi_ctx, lsquic_stream_t *stream,
                                 int is_push_promise) {
    (void)is_push_promise;
    cwist_h3_hset_t *hset = calloc(1, sizeof(*hset));
    if (!hset) return NULL;
    hset->stream = stream;
    cwist_h3_hset_track((cwist_http3_context *)hsi_ctx, hset);
    return hset;
}

static struct lsxpack_header *
cwist_h3_hsi_prepare(void *hset_p, struct lsxpack_header *xhdr, size_t req_space) {
    cwist_h3_hset_t *hset = hset_p;
    if (!hset) return NULL;

    /* lsquic calls us again with the same header when its initial output
     * buffer was too small.  Preserve the decoded name and grow only the
     * value capacity; reinitializing the slot here loses :path/Cookie. */
    if (xhdr) {
        if (req_space > LSXPACK_MAX_STRLEN || xhdr->name_offset < 0 ||
            (size_t)xhdr->name_offset >= sizeof(hset->decode_buf) ||
            req_space > sizeof(hset->decode_buf) - (size_t)xhdr->name_offset) {
            fprintf(stderr, "[HTTP/3] Rejecting oversized QPACK header resize (space=%zu, offset=%d)\n",
                    req_space, (int)xhdr->name_offset);
            return NULL;
        }
        xhdr->val_len = (lsxpack_strlen_t)req_space;
        return xhdr;
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
    cwist_h3_hset_t *hset = hset_p;
    /* A NULL header marks the end of a header block. */
    if (!hset || !xhdr)
        return 0;

    /* The QPACK decoder exposes the exact storage used by this completed
     * header. */
    size_t total = lsxpack_header_get_dec_size(xhdr);
    if (total > sizeof(hset->decode_buf) - hset->decode_off) {
        fprintf(stderr, "[HTTP/3] Rejecting oversized QPACK header (size=%zu, used=%zu)\n",
                total, hset->decode_off);
        return -1;
    }
    hset->decode_off += total;
    hset->count++;
    return 0;
}

static void cwist_h3_hsi_discard(void *hset_p) {
    cwist_h3_hset_untrack((cwist_h3_hset_t *)hset_p);
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

static void h3_setup_cmsg(struct msghdr *msg, char *cbuf, size_t cbuf_sz,
                          const struct lsquic_out_spec *spec, uint16_t gso_seg) {
    msg->msg_control = cbuf;
    msg->msg_controllen = cbuf_sz;
    memset(cbuf, 0, cbuf_sz);
    size_t ctl_len = 0;

#if defined(__linux__) && defined(UDP_SEGMENT)
    if (gso_seg > 0) {
        struct cmsghdr *cmsg = (struct cmsghdr *)(cbuf + ctl_len);
        cmsg->cmsg_level = SOL_UDP;
        cmsg->cmsg_type = UDP_SEGMENT;
        cmsg->cmsg_len = CMSG_LEN(sizeof(uint16_t));
        memcpy(CMSG_DATA(cmsg), &gso_seg, sizeof(uint16_t));
        ctl_len += CMSG_SPACE(sizeof(uint16_t));
    }
#else
    (void)gso_seg;
#endif

    if (spec->local_sa && spec->dest_sa) {
        if (spec->dest_sa->sa_family == AF_INET && spec->local_sa->sa_family == AF_INET) {
            struct in_addr addr = ((const struct sockaddr_in *)spec->local_sa)->sin_addr;
            if (addr.s_addr != INADDR_ANY) {
#if defined(__linux__) && defined(IP_PKTINFO)
                struct cmsghdr *cmsg = (struct cmsghdr *)(cbuf + ctl_len);
                cmsg->cmsg_level = IPPROTO_IP;
                cmsg->cmsg_type = IP_PKTINFO;
                cmsg->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
                struct in_pktinfo info = {0};
                info.ipi_spec_dst = addr;
                memcpy(CMSG_DATA(cmsg), &info, sizeof(info));
                ctl_len += CMSG_SPACE(sizeof(struct in_pktinfo));
#elif defined(IP_SENDSRCADDR)
                struct cmsghdr *cmsg = (struct cmsghdr *)(cbuf + ctl_len);
                cmsg->cmsg_level = IPPROTO_IP;
                cmsg->cmsg_type = IP_SENDSRCADDR;
                cmsg->cmsg_len = CMSG_LEN(sizeof(struct in_addr));
                memcpy(CMSG_DATA(cmsg), &addr, sizeof(addr));
                ctl_len += CMSG_SPACE(sizeof(struct in_addr));
#endif
            }
        } else if (spec->dest_sa->sa_family == AF_INET6 && spec->local_sa->sa_family == AF_INET6) {
            const struct in6_addr *addr6 = &((const struct sockaddr_in6 *)spec->local_sa)->sin6_addr;
            if (memcmp(addr6, &in6addr_any, sizeof(struct in6_addr)) != 0) {
#if defined(IPV6_PKTINFO)
                struct cmsghdr *cmsg = (struct cmsghdr *)(cbuf + ctl_len);
                cmsg->cmsg_level = IPPROTO_IPV6;
                cmsg->cmsg_type = IPV6_PKTINFO;
                cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
                struct in6_pktinfo info6 = {0};
                info6.ipi6_addr = *addr6;
                memcpy(CMSG_DATA(cmsg), &info6, sizeof(info6));
                ctl_len += CMSG_SPACE(sizeof(struct in6_pktinfo));
#endif
            }
        }
    }

#if defined(CWIST_H3_HAVE_ECN_CMSG)
    if (spec->ecn && spec->dest_sa) {
        if (spec->dest_sa->sa_family == AF_INET) {
            struct cmsghdr *cmsg = (struct cmsghdr *)(cbuf + ctl_len);
            cmsg->cmsg_level = IPPROTO_IP;
            cmsg->cmsg_type = IP_TOS;
            int tos = spec->ecn;
            cmsg->cmsg_len = CMSG_LEN(sizeof(tos));
            memcpy(CMSG_DATA(cmsg), &tos, sizeof(tos));
            ctl_len += CMSG_SPACE(sizeof(tos));
        }
#if defined(IPV6_TCLASS)
        else if (spec->dest_sa->sa_family == AF_INET6) {
            struct cmsghdr *cmsg = (struct cmsghdr *)(cbuf + ctl_len);
            cmsg->cmsg_level = IPPROTO_IPV6;
            cmsg->cmsg_type = IPV6_TCLASS;
            int tos = spec->ecn;
            cmsg->cmsg_len = CMSG_LEN(sizeof(tos));
            memcpy(CMSG_DATA(cmsg), &tos, sizeof(tos));
            ctl_len += CMSG_SPACE(sizeof(tos));
        }
#endif
    }
#endif

    msg->msg_controllen = ctl_len;
    if (ctl_len == 0) {
        msg->msg_control = NULL;
    }
}

static int h3_send_one(int udp_fd, const struct lsquic_out_spec *spec) {
    char ctrl[256];
    struct msghdr msg = {0};
    msg.msg_name = (void *)spec->dest_sa;
    msg.msg_namelen = (spec->dest_sa && spec->dest_sa->sa_family == AF_INET)
                      ? sizeof(struct sockaddr_in)
                      : sizeof(struct sockaddr_in6);
    msg.msg_iov = (struct iovec *)spec->iov;
    msg.msg_iovlen = spec->iovlen;
    h3_setup_cmsg(&msg, ctrl, sizeof(ctrl), spec, 0);
    return (int)sendmsg(udp_fd, &msg, MSG_DONTWAIT);
}

#if defined(__linux__)
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#ifndef SOL_UDP
#define SOL_UDP 17
#endif

static size_t h3_spec_len(const struct lsquic_out_spec *spec) {
    size_t n = 0;
    for (unsigned k = 0; k < spec->iovlen; k++) n += spec->iov[k].iov_len;
    return n;
}

static bool h3_same_dest(const struct lsquic_out_spec *a, const struct lsquic_out_spec *b) {
    if (!a->dest_sa || !b->dest_sa) return false;
    if (a->dest_sa->sa_family != b->dest_sa->sa_family) return false;
    if (a->local_sa != b->local_sa) {
        if (!a->local_sa || !b->local_sa) return false;
        if (a->local_sa->sa_family != b->local_sa->sa_family) return false;
        if (a->local_sa->sa_family == AF_INET) {
            const struct sockaddr_in *x = (const struct sockaddr_in *)a->local_sa;
            const struct sockaddr_in *y = (const struct sockaddr_in *)b->local_sa;
            if (x->sin_port != y->sin_port || x->sin_addr.s_addr != y->sin_addr.s_addr) return false;
        } else {
            const struct sockaddr_in6 *x = (const struct sockaddr_in6 *)a->local_sa;
            const struct sockaddr_in6 *y = (const struct sockaddr_in6 *)b->local_sa;
            if (x->sin6_port != y->sin6_port || memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(x->sin6_addr)) != 0) return false;
        }
    }
    if (a->dest_sa->sa_family == AF_INET) {
        const struct sockaddr_in *x = (const struct sockaddr_in *)a->dest_sa;
        const struct sockaddr_in *y = (const struct sockaddr_in *)b->dest_sa;
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    const struct sockaddr_in6 *x = (const struct sockaddr_in6 *)a->dest_sa;
    const struct sockaddr_in6 *y = (const struct sockaddr_in6 *)b->dest_sa;
    return x->sin6_port == y->sin6_port &&
           memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(x->sin6_addr)) == 0;
}
#endif

#if defined(__linux__)
/* Whether UDP GSO has been disabled at runtime.  Starts at -1 (unknown),
 * set to 1 if CWIST_H3_NO_GSO=1 env or if GSO sendmsg fails with a
 * hard error (ENOPROTOOPT, EIO, EMSGSIZE on a coalesced send).
 * 0 means GSO is confirmed working. */
static int h3_gso_state = -1; /* -1 = unknown, 0 = enabled, 1 = disabled */

static int h3_sendmmsg_batch(int udp_fd,
                              const struct lsquic_out_spec *specs,
                              unsigned i, unsigned n_specs) {
    struct mmsghdr msgs[64];
    char ctrl_bufs[64][256];

    while (i < n_specs) {
        unsigned batch = n_specs - i;
        if (batch > 64) batch = 64;

        for (unsigned k = 0; k < batch; k++) {
            const struct lsquic_out_spec *spec = &specs[i + k];
            struct msghdr *msg = &msgs[k].msg_hdr;
            memset(msg, 0, sizeof(*msg));
            msg->msg_name = (void *)spec->dest_sa;
            msg->msg_namelen = (spec->dest_sa && spec->dest_sa->sa_family == AF_INET)
                              ? sizeof(struct sockaddr_in)
                              : sizeof(struct sockaddr_in6);
            msg->msg_iov = (struct iovec *)spec->iov;
            msg->msg_iovlen = spec->iovlen;
            h3_setup_cmsg(msg, ctrl_bufs[k], sizeof(ctrl_bufs[k]), spec, 0);
            msgs[k].msg_len = 0;
        }

        int res = sendmmsg(udp_fd, msgs, batch, MSG_DONTWAIT);
        if (res > 0) {
            i += (unsigned)res;
            if ((unsigned)res < batch) break;
            continue;
        }
        if (res < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (i > 0) return (int)i;
            int nw = h3_send_one(udp_fd, &specs[i]);
            if (nw >= 0) { i++; continue; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
        break;
    }
    return (int)i;
}
#endif

static int cwist_h3_packets_out(void *ctx,
                                  const struct lsquic_out_spec *specs,
                                  unsigned n_specs) {
    int udp_fd = *(int *)ctx;
#if defined(__linux__)
    /* One-time GSO probe: check env var on first call. */
    if (__builtin_expect(h3_gso_state < 0, 0)) {
        const char *no_gso = getenv("CWIST_H3_NO_GSO");
        h3_gso_state = (no_gso && no_gso[0] == '1') ? 1 : 0;
    }

    if (!h3_gso_state) {
        /* GSO fast path: coalesce runs of equal-size datagrams to the same
         * peer into one sendmsg with UDP_SEGMENT. */
        unsigned i = 0;
        while (i < n_specs) {
            size_t seg = h3_spec_len(&specs[i]);
            unsigned run = 1;
            unsigned niov = specs[i].iovlen;
            if (seg > 0 && seg <= 65535) {
                /* Total payload must fit in one UDP datagram (65535 bytes);
                 * 65536/seg could allow an exactly-64KiB super-packet whose
                 * sendmsg fails with EMSGSIZE and would disable GSO. */
                unsigned max_segs = (unsigned)(65535 / seg);
                if (max_segs > 48) max_segs = 48;
                while (i + run < n_specs && run < max_segs &&
                       specs[i + run].ecn == specs[i].ecn &&
                       h3_same_dest(&specs[i], &specs[i + run]) &&
                       h3_spec_len(&specs[i + run]) == seg &&
                       niov + specs[i + run].iovlen <= 128) {
                    niov += specs[i + run].iovlen;
                    run++;
                }
            }
            if (run >= 2) {
                struct iovec iov[128];
                unsigned n = 0;
                for (unsigned k = 0; k < run; k++)
                    for (unsigned m = 0; m < specs[i + k].iovlen; m++)
                        iov[n++] = specs[i + k].iov[m];

                char ctrl[512];
                struct msghdr msg = {0};
                msg.msg_name = (void *)specs[i].dest_sa;
                msg.msg_namelen = (specs[i].dest_sa && specs[i].dest_sa->sa_family == AF_INET)
                                  ? sizeof(struct sockaddr_in)
                                  : sizeof(struct sockaddr_in6);
                msg.msg_iov = iov;
                msg.msg_iovlen = n;
                h3_setup_cmsg(&msg, ctrl, sizeof(ctrl), &specs[i], (uint16_t)seg);

                ssize_t nw = sendmsg(udp_fd, &msg, MSG_DONTWAIT);
                if (nw >= 0) {
                    i += run;
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                /* GSO rejected: permanently disable and fall back to sendmmsg. */
                h3_gso_state = 1;
                return h3_sendmmsg_batch(udp_fd, specs, i, n_specs);
            }
            /* Single packet: send directly. */
            int nw = h3_send_one(udp_fd, &specs[i]);
            if (nw < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return (int)i;
                if (errno == ECONNREFUSED || errno == ENETUNREACH ||
                    errno == EHOSTUNREACH || errno == EMSGSIZE) {
                    i++;
                    continue;
                }
                return (i > 0) ? (int)i : -1;
            }
            i++;
        }
        return (int)i;
    }

    /* sendmmsg fallback path (GSO disabled). */
    return h3_sendmmsg_batch(udp_fd, specs, 0, n_specs);
#else
    unsigned i = 0;
    for (; i < n_specs; ++i) {
        int nw = h3_send_one(udp_fd, &specs[i]);
        if (nw < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return (int)i;
            if (errno == ECONNREFUSED || errno == ENETUNREACH ||
                errno == EHOSTUNREACH || errno == EMSGSIZE)
                continue;
            return (i > 0) ? (int)i : -1;
        }
    }
    return (int)i;
#endif
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

static int cwist_h3_log_stderr(void *ctx, const char *buf, size_t len) {
    (void)ctx;
    return (int)fwrite(buf, 1, len, stderr);
}

static lsquic_conn_ctx_t *cwist_h3_on_new_conn(void *stream_if_ctx,
                                                lsquic_conn_t *conn) {
    cwist_http3_context *h3_ctx = stream_if_ctx;
    lsquic_conn_set_ctx(conn, (lsquic_conn_ctx_t *)h3_ctx);
    return (lsquic_conn_ctx_t *)h3_ctx;
}

static void cwist_h3_on_conn_closed(lsquic_conn_t *conn) {
    if (!conn) return;
    char errbuf[256] = {0};
    enum LSQUIC_CONN_STATUS status = lsquic_conn_status(conn, errbuf, sizeof(errbuf));
    fprintf(stderr, "[HTTP/3] Conn close status=%d msg=%s\n", (int)status,
            errbuf[0] ? errbuf : "(none)");
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

    /* RFC 9218 Section 6: a server MUST NOT send a PRIORITY_UPDATE frame for
     * a request stream.  lsquic_stream_set_priority() emits exactly that
     * frame when ext priorities are negotiated, and strict clients (Firefox/
     * neqo) close the connection with H3_FRAME_UNEXPECTED.  Keep the default
     * scheduling priority instead of touching the stream. */
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
            req->query_params = cwist_query_map_create_in_arena(req->arena);
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

static int h3_header_name_char_is_valid(unsigned char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '!' || c == '#' || c == '$' || c == '%' ||
           c == '&' || c == '\'' || c == '*' || c == '+' ||
           c == '-' || c == '.' || c == '^' || c == '_' ||
           c == '`' || c == '|' || c == '~';
}

int cwist_http3_normalize_response_header_name(const char *name,
                                               char *out,
                                               size_t out_len) {
    if (!name || !out || out_len == 0) return -1;

    size_t len = strlen(name);
    if (len == 0 || len >= out_len || name[0] == ':') return -1;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!h3_header_name_char_is_valid(c)) return -1;
        out[i] = (char)tolower(c);
    }
    out[len] = '\0';
    return 0;
}

int cwist_http3_response_header_value_is_safe(const char *value) {
    if (!value) return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        /* RFC 9113/9114 field-value grammar: no CTLs (incl. CR/LF) or DEL;
         * clients reject such responses as malformed. */
        if (*p < 0x20 || *p == 0x7f) return 0;
    }
    return 1;
}

static void h3_apply_header(cwist_http_request *req,
                            const char *name, const char *value) {
    if (strcmp(name, ":method") == 0) {
        req->method = cwist_http_string_to_method(value);
    } else if (strcmp(name, ":path") == 0) {
        h3_parse_path(req, value);
    } else if (strcmp(name, ":authority") == 0 || strcmp(name, "host") == 0) {
        if (!cwist_http_header_get(req->headers, "host")) {
            cwist_http_header_add(&req->headers, "host", value);
        }
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
        /* RFC 9218 Extensible Priorities: u=urgency, i=incremental.
         * Record the header only: calling lsquic_stream_set_priority() here
         * would emit a PRIORITY_UPDATE frame for a request stream, which a
         * server MUST NOT send (strict clients abort with
         * H3_FRAME_UNEXPECTED). */
        cwist_http_header_add(&req->headers, name, value);
    } else if (name[0] != ':') {
        /* Any other non-pseudo header */
        cwist_http_header_add(&req->headers, name, value);
    }
}

/* HTTP/3 delivers an ordered QUIC byte stream, but an application-level
 * sequenced body can be split at arbitrary read boundaries.  Buffer just one
 * wire chunk, then hand complete chunks to the common TASFA-style ARQ
 * assembler. */
static int h3_seq_append_and_feed(h3_stream_ctx_t *st,
                                  const unsigned char *data, size_t len) {
    if (len > SIZE_MAX - st->seq_len) return -1;
    size_t need = st->seq_len + len;
    if (need > st->seq_cap) {
        size_t cap = st->seq_cap ? st->seq_cap : 4096;
        while (cap < need) {
            if (cap > (CWIST_SEQ_HEADER_SIZE + UINT16_MAX) / 2) {
                cap = CWIST_SEQ_HEADER_SIZE + UINT16_MAX;
                break;
            }
            cap *= 2;
        }
        if (cap < need || cap > CWIST_SEQ_HEADER_SIZE + UINT16_MAX) return -1;
        unsigned char *tmp = realloc(st->seq_buf, cap);
        if (!tmp) return -1;
        st->seq_buf = tmp;
        st->seq_cap = cap;
    }
    memcpy(st->seq_buf + st->seq_len, data, len);
    st->seq_len += len;

    while (st->seq_len >= CWIST_SEQ_HEADER_SIZE) {
        size_t payload_len = ((size_t)st->seq_buf[4] << 8) | st->seq_buf[5];
        size_t chunk_len = CWIST_SEQ_HEADER_SIZE + payload_len;
        if (payload_len == 0 || st->seq_len < chunk_len) break;
        cwist_seq_chunk_t chunk;
        if (!cwist_seq_chunk_parse(st->seq_buf, chunk_len, &chunk)) return -1;
        if (!st->body_assembler) {
            st->body_assembler = cwist_seq_assembler_create_limited(CWIST_HTTP_MAX_BODY_SIZE);
            if (!st->body_assembler) return -1;
        }
        if (!cwist_seq_assembler_feed(st->body_assembler, &chunk)) return -1;
        memmove(st->seq_buf, st->seq_buf + chunk_len, st->seq_len - chunk_len);
        st->seq_len -= chunk_len;
    }
    return 0;
}

/* Reject a request whose header block violates RFC 9114 Section 4.3.1.
 * lsquic exposes no stream-reset API in this baseline, so enforcement is a
 * 400 response followed by closing both stream directions (mirrors the
 * existing 413 path), and the request is never dispatched to the handler. */
static void h3_reject_malformed_request(lsquic_stream_t *stream,
                                        h3_stream_ctx_t *st) {
    st->malformed = 1;
    st->headers_done = 1;
    if (!st->res) st->res = cwist_http_response_create();
    if (st->res) {
        st->res->status_code = CWIST_HTTP_BAD_REQUEST;
        if (st->res->body) {
            cwist_sstring_assign(st->res->body,
                "{\"error\":\"malformed request\"}");
        }
        if (st->res->headers) {
            cwist_http_header_add(&st->res->headers,
                                  "Content-Type", "application/json");
        }
    }
    st->response_ready = 1;
    lsquic_stream_wantread(stream, 0);
    lsquic_stream_shutdown(stream, 0);
    lsquic_stream_wantwrite(stream, 1);
}

static bool h3_process_stream_headers(lsquic_stream_t *stream, h3_stream_ctx_t *st) {
    if (st->headers_done) return true;
    void *hset = lsquic_stream_get_hset(stream);
    if (!hset) return false;
    cwist_h3_hset_t *hs = (cwist_h3_hset_t *)hset;
    for (size_t i = 0; i < hs->count; ++i) {
        const struct lsxpack_header *xhdr = &hs->headers[i];
        const char *raw_name  = lsxpack_header_get_name(xhdr);
        const char *raw_value = lsxpack_header_get_value(xhdr);
        size_t name_len = xhdr->name_len;
        size_t value_len = xhdr->val_len;
        if (raw_name && raw_value && name_len > 0 &&
            name_len <= 1024 && value_len <= H3_DECODE_BUF_SIZE - 1) {
            /* lsxpack exposes counted slices, not C strings. */
            char *name = malloc(name_len + 1);
            char *value = malloc(value_len + 1);
            if (!name || !value) {
                free(name);
                free(value);
                lsquic_stream_close(stream);
                return false;
            }
            memcpy(name, raw_name, name_len);
            name[name_len] = '\0';
            memcpy(value, raw_value, value_len);
            value[value_len] = '\0';
            if (name[0] == ':') {
                /* RFC 9114 Section 4.3.1: pseudo-headers precede regular
                 * headers, appear at most once, and only the defined set is
                 * valid in requests. */
                unsigned bit = 0;
                if (strcmp(name, ":method") == 0) bit = H3_PSEUDO_METHOD;
                else if (strcmp(name, ":scheme") == 0) bit = H3_PSEUDO_SCHEME;
                else if (strcmp(name, ":path") == 0) bit = H3_PSEUDO_PATH;
                else if (strcmp(name, ":authority") == 0) bit = H3_PSEUDO_AUTHORITY;
                else if (strcmp(name, ":protocol") == 0) bit = H3_PSEUDO_PROTOCOL;
                if (bit == 0 || st->seen_regular_header ||
                    (st->pseudo_seen & bit)) {
                    free(value);
                    free(name);
                    h3_reject_malformed_request(stream, st);
                    return false;
                }
                st->pseudo_seen |= bit;
                if (bit == H3_PSEUDO_METHOD && strcmp(value, "CONNECT") == 0) {
                    st->is_connect = 1;
                }
                if (bit == H3_PSEUDO_PATH && value_len == 0) {
                    st->saw_empty_path = 1;
                }
            } else {
                st->seen_regular_header = 1;
            }
            h3_apply_header(st->req, name, value);
#ifdef CWIST_WEBTRANSPORT
            if (strcmp(name, ":protocol") == 0 && strcmp(value, "webtransport") == 0) {
                if (st->req && st->req->method == CWIST_HTTP_CONNECT) {
                    st->is_webtransport = 1;
                }
            }
#endif
            free(value);
            free(name);
        }
    }
    /* RFC 9114 Section 4.3.1 completeness rules, evaluated once the whole
     * header block has been seen. */
    if (st->is_connect && !(st->pseudo_seen & H3_PSEUDO_PROTOCOL)) {
        /* Plain CONNECT: :scheme and :path MUST be omitted, :authority is
         * required. */
        if ((st->pseudo_seen & (H3_PSEUDO_SCHEME | H3_PSEUDO_PATH)) ||
            !(st->pseudo_seen & H3_PSEUDO_AUTHORITY)) {
            h3_reject_malformed_request(stream, st);
            return false;
        }
    } else if (st->is_connect) {
        /* Extended CONNECT (RFC 9220, e.g. WebTransport): :scheme, :path and
         * :authority are all required. */
        if (!(st->pseudo_seen & H3_PSEUDO_SCHEME) ||
            !(st->pseudo_seen & H3_PSEUDO_PATH) ||
            !(st->pseudo_seen & H3_PSEUDO_AUTHORITY)) {
            h3_reject_malformed_request(stream, st);
            return false;
        }
    } else {
        /* Normal request: :method, :scheme and :path are mandatory,
         * :authority (or an equivalent Host header) is required for https,
         * and :protocol is only legal on extended CONNECT. */
        if (!(st->pseudo_seen & H3_PSEUDO_METHOD) ||
            !(st->pseudo_seen & H3_PSEUDO_SCHEME) ||
            !(st->pseudo_seen & H3_PSEUDO_PATH) ||
            (st->pseudo_seen & H3_PSEUDO_PROTOCOL) ||
            (!(st->pseudo_seen & H3_PSEUDO_AUTHORITY) &&
             !cwist_http_header_get(st->req->headers, "host"))) {
            h3_reject_malformed_request(stream, st);
            return false;
        }
        /* An empty :path is only legal for OPTIONS (asterisk-form). */
        if (st->saw_empty_path &&
            (!st->req || st->req->method != CWIST_HTTP_OPTIONS)) {
            h3_reject_malformed_request(stream, st);
            return false;
        }
    }
    st->headers_done = 1;
    char *seq_header = cwist_http_header_get(st->req->headers,
                                              "x-cwist-sequenced-data");
    st->sequenced_data = seq_header &&
        (strcmp(seq_header, "1") == 0 || strcasecmp(seq_header, "true") == 0);
    return true;
}

/* RFC 9110: methods with the idempotent property are safe to replay, which
 * makes them acceptable for 0-RTT delivery. */
static bool h3_method_is_idempotent(cwist_http_method_t method) {
    switch (method) {
    case CWIST_HTTP_GET:
    case CWIST_HTTP_HEAD:
    case CWIST_HTTP_PUT:
    case CWIST_HTTP_DELETE:
    case CWIST_HTTP_OPTIONS:
        return true;
    default:
        return false;
    }
}

static void cwist_h3_on_read(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    h3_stream_ctx_t *st = (h3_stream_ctx_t *)st_h;
    if (!st) return;

    unsigned char buf[8192];
    ssize_t nread;

    h3_process_stream_headers(stream, st);
    if (st->malformed) return;

    while ((nread = lsquic_stream_read(stream, buf, sizeof(buf))) > 0) {
        if (st->sequenced_data) {
            if (h3_seq_append_and_feed(st, buf, (size_t)nread) != 0) {
                lsquic_stream_close(stream);
                return;
            }
            continue;
        }
        size_t need = st->body_len + (size_t)nread;
        if (need > CWIST_HTTP_MAX_BODY_SIZE) {
            /* Body too large: answer 413 with a small JSON body instead of a
             * bare stream close, so the client sees an explicit status
             * (RFC 9110 Section 15.5.14). */
            if (!st->res) st->res = cwist_http_response_create();
            if (st->res) {
                st->res->status_code = 413; /* Payload Too Large */
                if (st->res->body) {
                    cwist_sstring_assign(st->res->body,
                        "{\"error\":\"payload too large\"}");
                }
                if (st->res->headers) {
                    cwist_http_header_add(&st->res->headers,
                                          "Content-Type", "application/json");
                }
                st->response_ready = 1;
                lsquic_stream_wantread(stream, 0);
                lsquic_stream_shutdown(stream, 0);
                lsquic_stream_wantwrite(stream, 1);
            } else {
                lsquic_stream_close(stream);
            }
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
        h3_process_stream_headers(stream, st);

        if (st->malformed) return;

        if (!st->headers_done) {
            /* Malformed request: no headers before FIN */
            st->res = cwist_http_response_create();
            if (st->res) {
                st->res->status_code = CWIST_HTTP_BAD_REQUEST;
                if (st->res->body) {
                    cwist_sstring_assign(st->res->body,
                        "{\"error\":\"malformed request\"}");
                }
                if (st->res->headers) {
                    cwist_http_header_add(&st->res->headers,
                                          "Content-Type", "application/json");
                }
            }
            st->response_ready = 1;
            lsquic_stream_wantread(stream, 0);
            lsquic_stream_shutdown(stream, 0);
            lsquic_stream_wantwrite(stream, 1);
            return;
        }

        if (st->sequenced_data) {
            const uint8_t *assembled = NULL;
            size_t assembled_len = 0;
            if (st->seq_len != 0 || !st->body_assembler ||
                !cwist_seq_assembler_get_data(st->body_assembler, &assembled, &assembled_len)) {
                /* Do not dispatch a partial request. */
                st->res = cwist_http_response_create();
                if (st->res) {
                    st->res->status_code = CWIST_HTTP_BAD_REQUEST;
                    cwist_http_header_add(&st->res->headers, "x-cwist-retry", "1");
                    if (st->res->body) {
                        cwist_sstring_assign(st->res->body,
                            "{\"error\":\"incomplete sequenced body, retry\"}");
                    }
                    cwist_http_header_add(&st->res->headers,
                                          "Content-Type", "application/json");
                }
                st->response_ready = 1;
                lsquic_stream_wantread(stream, 0);
                lsquic_stream_shutdown(stream, 0);
                lsquic_stream_wantwrite(stream, 1);
                return;
            }
            if (assembled_len == 0) {
                /* realloc(p, 0) may legally return NULL; do not mistake an
                 * empty assembled body for an allocation failure (which used
                 * to reset the stream instead of dispatching the request). */
                free(st->body);
                st->body = NULL;
                st->body_len = 0;
                st->body_cap = 0;
            } else {
                st->body = realloc(st->body, assembled_len);
                if (!st->body) {
                    lsquic_stream_close(stream);
                    return;
                }
                memcpy(st->body, assembled, assembled_len);
                st->body_len = assembled_len;
                st->body_cap = assembled_len;
            }
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
                    lsquic_stream_shutdown(stream, 0);
                    lsquic_stream_wantwrite(stream, 0);
                    return;
                }
                /* Accept failed: falling through with the default 200/empty
                 * response used to send the client a silent 0-byte body. */
                CWIST_LOG_WARN("[HTTP/3] WebTransport accept failed; answering 500");
                st->res->status_code = CWIST_HTTP_INTERNAL_ERROR;
                if (st->res->body && st->res->body->size == 0) {
                    cwist_sstring_assign(st->res->body,
                        "{\"error\":\"webtransport accept failed\"}");
                }
                if (st->res->headers) {
                    cwist_http_header_add(&st->res->headers,
                                          "Content-Type", "application/json");
                }
            } else
#endif
            if (h3_ctx && h3_ctx->handler) {
                /* 0-RTT replay guard: requests delivered while the QUIC
                 * handshake is still in progress arrived as early data.
                 * lsquic (this baseline) exposes no
                 * lsquic_conn_is_early_data_accepted()-style query, so the
                 * guard uses the public lsquic_conn_status() instead. */
                bool is_early_data = h3_ctx->early_data_enabled &&
                    h3_ctx->early_data_guard &&
                    lsquic_conn_status(lsquic_stream_conn(stream), NULL, 0)
                        == LSCONN_ST_HSK_IN_PROGRESS;
                if (is_early_data && st->req &&
                    !h3_method_is_idempotent(st->req->method)) {
                    /* RFC 8470: refuse replayable non-idempotent early data
                     * so the client retries after the handshake.  Attach a
                     * body: without one the response goes out as HEADERS+FIN
                     * with no content-length, which some clients surface as
                     * an empty (0-byte) reply instead of status 425. */
                    st->res->status_code = 425; /* Too Early */
                    if (st->res->body) {
                        cwist_sstring_assign(st->res->body,
                            "{\"error\":\"too early, retry after handshake\"}");
                    }
                    if (st->res->headers) {
                        cwist_http_header_add(&st->res->headers,
                                              "Content-Type", "application/json");
                    }
                } else {
                    h3_ctx->handler(h3_ctx->user_ctx, st->req, st->res);
                }
            }
        }
        st->response_ready = 1;
        lsquic_stream_wantread(stream, 0);
        lsquic_stream_shutdown(stream, 0);
        lsquic_stream_wantwrite(stream, 1);
    } else if (nread < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        lsquic_stream_close(stream);
    }
}

/* RFC 9110 Section 8.6: content-length must be a non-empty run of digits.
 * Anything else from a handler (whitespace, junk, negative, empty) is
 * rejected rather than forwarded into the response headers. */
static bool h3_content_length_is_valid(const char *cl) {
    if (!cl || cl[0] < '0' || cl[0] > '9') return false;
    for (const char *p = cl + 1; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

/* RFC 9114 Section 4.1.2: 1xx/204/304 responses carry no content, and
 * content-length on them makes the message malformed at connection level. */
static bool h3_status_forbids_body(int status_code) {
    return (status_code >= 100 && status_code < 200) ||
           status_code == 204 || status_code == 304;
}

static void cwist_h3_on_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    h3_stream_ctx_t *st = (h3_stream_ctx_t *)st_h;
    if (!st || !st->response_ready) return;
    if (!st->res) {
        /* Response allocation failed upstream; reset instead of dereferencing
         * NULL (which used to take down every stream on the connection). */
        lsquic_stream_close(stream);
        return;
    }

    if (st->write_state == 0) {
        struct lsxpack_header headers_arr[H3_MAX_RESPONSE_HEADERS];
        char hbuf[H3_RESPONSE_HEADER_BUF_SIZE];
        size_t hbuf_off = 0;
        size_t hdr_count = 0;

        bool bodyless = h3_status_forbids_body(st->res->status_code);
        bool is_head = st->req && st->req->method == CWIST_HTTP_HEAD;

        /* Handler attached a body source that cannot produce bytes
         * (NULL pointer body / invalid file fd with a non-zero length).
         * Sending HEADERS would announce (or imply) a body we cannot
         * deliver; close the stream so the client sees an explicit error
         * instead of a silent empty 200. */
        if (!bodyless && !is_head &&
            ((st->res->is_ptr_body && !st->res->ptr_body &&
              st->res->ptr_body_len > 0) ||
             (st->res->use_file_stream && st->res->file_stream_fd < 0 &&
              st->res->file_stream_len > 0))) {
            CWIST_LOG_WARN("[HTTP/3] unusable response body source; resetting stream");
            lsquic_stream_close(stream);
            return;
        }

        /* :status */
        if (st->res->status_code < 200 || st->res->status_code > 999) {
            /* RFC 9110 Section 15: a final response needs a status in
             * 200-999 (RFC 9114 Section 4.1.2 requires three digits).
             * A handler that leaves the status unset, picks 1xx (an
             * interim response, illegal as the complete answer), or an
             * out-of-range value gets coerced to 500 instead of putting
             * a malformed :status on the wire. */
            CWIST_LOG_WARN("[HTTP/3] invalid status %d; coercing to 500",
                           st->res->status_code);
            st->res->status_code = CWIST_HTTP_INTERNAL_ERROR;
            bodyless = false;
            if (!is_head && st->res->body && st->res->body->size == 0 &&
                !st->res->is_ptr_body && !st->res->use_file_stream) {
                cwist_sstring_assign(st->res->body,
                    "{\"error\":\"internal server error\"}");
            }
        }
        int status_code = st->res->status_code;
        char status_str[16];
        snprintf(status_str, sizeof(status_str), "%d", status_code);
        size_t slen = strlen(status_str);
        if (hdr_count < H3_MAX_RESPONSE_HEADERS && slen > 0 && hbuf_off + 7 + 2 + slen <= sizeof(hbuf)) {
            memcpy(hbuf + hbuf_off, ":status", 7);
            memcpy(hbuf + hbuf_off + 9, status_str, slen);
            lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off,
                                       0, 7, 9, slen);
            hbuf_off += 9 + slen;
            hdr_count++;
        }

        /* content-length */
        size_t body_len = 0;
        if (st->res->use_file_stream) {
            if (st->res->file_stream_fd >= 0) body_len = st->res->file_stream_len;
        }
        else if (st->res->is_ptr_body) body_len = st->res->ptr_body ? st->res->ptr_body_len : 0;
        else if (st->res->body) body_len = st->res->body->size;

        const char *user_cl = st->res->headers
            ? cwist_http_header_get(st->res->headers, "content-length") : NULL;

        if (!bodyless && (body_len > 0 || is_head) && hdr_count < H3_MAX_RESPONSE_HEADERS) {
            /* For HEAD, content-length describes the would-be GET body: keep
             * the handler-provided value when present (e.g. static file size
             * with an empty body), otherwise compute it like a GET.  For all
             * other methods the computed body length is authoritative: it is
             * exactly what the write path below will send, so trusting a
             * divergent handler value would announce bytes never delivered. */
            char cl_str[32];
            const char *cl_val = NULL;
            if (is_head && user_cl) {
                if (h3_content_length_is_valid(user_cl)) {
                    cl_val = user_cl;
                } else {
                    /* Malformed handler content-length must not reach the
                     * wire; fall back to the computed body length. */
                    CWIST_LOG_WARN("[HTTP/3] dropping invalid HEAD content-length '%s'",
                                   user_cl);
                }
            }
            if (!cl_val) {
                snprintf(cl_str, sizeof(cl_str), "%zu", body_len);
                cl_val = cl_str;
            }
            if (cl_val && cl_val[0] != '\0') {
                size_t cl_name_len = 14;
                size_t cl_val_len  = strlen(cl_val);
                size_t total = cl_name_len + 2 + cl_val_len;
                if (cl_val_len > 0 && hbuf_off + total <= sizeof(hbuf)) {
                    memcpy(hbuf + hbuf_off, "content-length", cl_name_len);
                    memcpy(hbuf + hbuf_off + cl_name_len + 2, cl_val, cl_val_len);
                    lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off,
                                               0, cl_name_len, cl_name_len + 2, cl_val_len);
                    hbuf_off += total;
                    hdr_count++;
                }
            }
        }

        /* content-type (if body is present or HEAD request) */
        if (st->res->headers && (body_len > 0 || is_head)) {
            char *ct = cwist_http_header_get(st->res->headers, "content-type");
            if (ct && ct[0] != '\0' && hdr_count < H3_MAX_RESPONSE_HEADERS) {
                size_t klen = 12;
                size_t vlen = strlen(ct);
                if (vlen > 0 && hbuf_off + klen + 2 + vlen <= sizeof(hbuf)) {
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
        while (node && hdr_count < H3_MAX_RESPONSE_HEADERS) {
            if (node->key && node->key->data && node->key->size > 0 &&
                node->value && node->value->data) {
                char h3_name[256];
                if (cwist_http3_normalize_response_header_name(node->key->data,
                                                               h3_name,
                                                               sizeof(h3_name)) != 0 ||
                    h3_name[0] == '\0' ||
                    strlen(node->value->data) != node->value->size ||
                    !cwist_http3_response_header_value_is_safe(node->value->data)) {
                    node = node->next;
                    continue;
                }

                if (strcmp(h3_name, "content-length") == 0 ||
                    strcmp(h3_name, "content-type") == 0) {
                    node = node->next;
                    continue;
                }
                /* RFC 9114 §4.2: connection-specific fields are malformed in
                 * HTTP/3; te is only allowed with the value "trailers". */
                if (strcmp(h3_name, "connection") == 0 ||
                    strcmp(h3_name, "keep-alive") == 0 ||
                    strcmp(h3_name, "proxy-connection") == 0 ||
                    strcmp(h3_name, "transfer-encoding") == 0 ||
                    strcmp(h3_name, "upgrade") == 0 ||
                    (strcmp(h3_name, "te") == 0 &&
                     strcasecmp(node->value->data, "trailers") != 0)) {
                    node = node->next;
                    continue;
                }

                size_t klen = strlen(h3_name);
                size_t vlen = node->value->size;
                if (klen > 0 && hbuf_off + klen + 2 + vlen <= sizeof(hbuf)) {
                    memcpy(hbuf + hbuf_off, h3_name, klen);
                    memcpy(hbuf + hbuf_off + klen + 2, node->value->data, vlen);
                    lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off,
                                               0, klen, klen + 2, vlen);
                    hbuf_off += klen + 2 + vlen;
                    hdr_count++;
                }
            }
            node = node->next;
        }

        if (status_code == CWIST_HTTP_OK && !bodyless && !is_head &&
            body_len == 0) {
            /* Legal per RFC 9110, but a 200 with no body and no
             * content-length usually means a handler forgot to assign one;
             * make it diagnosable in the logs. */
            CWIST_LOG_WARN("[HTTP/3] handler produced 200 with empty body");
        }

        size_t initial_body_len = 0;
        if (st->res->use_file_stream && st->res->file_stream_fd >= 0) {
            initial_body_len = st->res->file_stream_len;
        } else if (st->res->is_ptr_body && st->res->ptr_body) {
            initial_body_len = st->res->ptr_body_len;
        } else if (st->res->body) {
            initial_body_len = st->res->body->size;
        }
        int eos = (bodyless || is_head || initial_body_len == 0) ? 1 : 0;

        lsquic_http_headers_t headers = {
            .count = (unsigned)hdr_count,
            .headers = headers_arr,
        };
        if (lsquic_stream_send_headers(stream, &headers, eos) != 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                lsquic_stream_wantwrite(stream, 1);
                return;
            }
            lsquic_stream_close(stream);
            return;
        }
        if (eos) {
            /* NB: the eos argument of lsquic_stream_send_headers is ignored
             * for IETF QUIC, so an empty body must be finished with an
             * explicit shutdown(1) here.  Relying on eos leaves every
             * empty-body response (204s, 304s, HEAD, error statuses) without
             * FIN, which browsers surface as a protocol error — timing- and
             * RTT-dependent because of header-block stashing under low cwnd. */
            lsquic_stream_shutdown(stream, 1);
            st->write_state = 2;
            lsquic_stream_wantread(stream, 1);
            return;
        }
        st->write_state = 1;
        lsquic_stream_wantwrite(stream, 1);
        return;
    }

    if (st->write_state == 1 && st->res) {
        bool bodyless = h3_status_forbids_body(st->res->status_code);
        bool is_head = st->req && st->req->method == CWIST_HTTP_HEAD;
        size_t body_len = 0;
        const char *body_data = NULL;

        if (st->res->use_file_stream) {
            /* Fill the congestion window on each callback: loop chunk writes
             * until the stream says EAGAIN instead of one 64 KiB chunk per
             * tick. */
            while (st->res->file_stream_fd >= 0 && st->body_sent < st->res->file_stream_len) {
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
                    if (nw == 0) {
                        lsquic_stream_wantwrite(stream, 1);
                        return;
                    }
                    st->send_xor ^= h3_xor_bytes((const unsigned char *)file_buf, (size_t)nw);
                    st->body_sent += (size_t)nw;
                    if (nw < nr) {
                        /* Stream buffer full for now; come back when writable. */
                        lsquic_stream_wantwrite(stream, 1);
                        return;
                    }
                    continue;
                } else if (nr == 0) {
                    /* Short EOF (file truncated after fstat): the announced
                     * content-length can no longer be honoured, and finishing
                     * anyway would be a malformed-message error at the
                     * client.  Reset the stream instead. */
                    lsquic_stream_close(stream);
                    return;
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        /* Retry next tick */
                        lsquic_stream_wantwrite(stream, 1);
                        return;
                    }
                    lsquic_stream_close(stream);
                    return;
                }
            }
            if (st->res->file_stream_fd >= 0)
                body_len = st->res->file_stream_len;
        } else if (st->res->is_ptr_body) {
            if (!st->res->ptr_body && st->res->ptr_body_len > 0) {
                /* Announced a body we cannot produce; spinning here with
                 * wantwrite armed would hang the stream forever. */
                lsquic_stream_close(stream);
                return;
            }
            body_len = st->res->ptr_body_len;
            body_data = (const char *)st->res->ptr_body;
        } else if (st->res->body) {
            body_len = st->res->body->size;
            body_data = st->res->body->data;
        }

        /* Skip payload writing entirely if body is empty, forbidden, or already fully sent */
        if (bodyless || is_head || body_len == 0 || st->body_sent >= body_len) {
            st->write_state = 2;
            lsquic_stream_shutdown(stream, 1);
            lsquic_stream_wantread(stream, 1);
            return;
        }

        while (body_data && body_len > 0 && st->body_sent < body_len) {
            ssize_t n = lsquic_stream_write(stream, body_data + st->body_sent,
                                            body_len - st->body_sent);
            if (n > 0) {
                st->send_xor ^= h3_xor_bytes((const unsigned char *)(body_data + st->body_sent), (size_t)n);
                st->body_sent += (size_t)n;
                if (st->body_sent < body_len) {
                    lsquic_stream_wantwrite(stream, 1);
                }
                continue;
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    lsquic_stream_wantwrite(stream, 1);
                    return;
                }
                lsquic_stream_close(stream);
                return;
            }
            /* n == 0: stream buffer full for now; re-arm wantwrite and come back */
            lsquic_stream_wantwrite(stream, 1);
            return;
        }

        if (st->body_sent >= body_len) {
            st->write_state = 2;
            lsquic_stream_shutdown(stream, 1);
            lsquic_stream_wantread(stream, 1);
        } else {
            lsquic_stream_wantwrite(stream, 1);
        }
    }
}

static void cwist_h3_on_close(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    h3_stream_ctx_t *st = (h3_stream_ctx_t *)st_h;
    if (st) {
        cwist_http_request_destroy(st->req);
        cwist_http_response_destroy(st->res);
        free(st->body);
        cwist_seq_assembler_destroy(st->body_assembler);
        free(st->seq_buf);
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

/* --- Shared session ticket key -------------------------------------------
 * Session resumption (and therefore 0-RTT early data) requires the server to
 * issue resumable tickets.  Mirrors the prefork-safe pattern used by the
 * HTTPS stack (src/net/http/https.c): one random key generated at context
 * creation and installed via an explicit ticket-key callback, so tickets
 * issued by one worker resume on any other (SO_REUSEPORT scatters
 * connections across workers).  The key lives for the process lifetime.
 * ------------------------------------------------------------------------- */
typedef struct cwist_h3_ticket_key {
    unsigned char name[16];     /* key_name sent in the ticket */
    unsigned char aes_key[32];  /* AES-256-CBC encryption key  */
    unsigned char hmac_key[32]; /* HMAC-SHA-256 MAC key        */
} cwist_h3_ticket_key;

static int g_h3_ticket_key_ex_data_idx = -1;
static pthread_once_t g_h3_ticket_key_ex_data_once = PTHREAD_ONCE_INIT;

static void cwist_h3_ticket_key_ex_data_init(void) {
    g_h3_ticket_key_ex_data_idx = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}

static int cwist_h3_ticket_key_cb(SSL *ssl, uint8_t *key_name, uint8_t *iv,
                                  EVP_CIPHER_CTX *ectx, HMAC_CTX *hctx, int encrypt) {
    SSL_CTX *ssl_ctx = ssl ? SSL_get_SSL_CTX(ssl) : NULL;
    const cwist_h3_ticket_key *key = NULL;
    if (ssl_ctx && g_h3_ticket_key_ex_data_idx >= 0) {
        key = (const cwist_h3_ticket_key *)SSL_CTX_get_ex_data(ssl_ctx, g_h3_ticket_key_ex_data_idx);
    }
    if (!key) return 0;

    if (encrypt) {
        memcpy(key_name, key->name, sizeof(key->name));
        if (RAND_bytes(iv, 16) != 1) return 0;
        if (EVP_EncryptInit_ex(ectx, EVP_aes_256_cbc(), NULL, key->aes_key, iv) != 1) return 0;
        if (HMAC_Init_ex(hctx, key->hmac_key, sizeof(key->hmac_key), EVP_sha256(), NULL) != 1) return 0;
        return 1;
    }

    if (memcmp(key_name, key->name, sizeof(key->name)) != 0) {
        return 0; /* Unknown key: decline the ticket, do a full handshake. */
    }
    if (EVP_DecryptInit_ex(ectx, EVP_aes_256_cbc(), NULL, key->aes_key, iv) != 1) return 0;
    if (HMAC_Init_ex(hctx, key->hmac_key, sizeof(key->hmac_key), EVP_sha256(), NULL) != 1) return 0;
    return 1;
}

/**
 * @brief Arm the server session cache and the shared ticket key so the
 *        context issues resumable tickets (prerequisite for 0-RTT).
 * @param ssl_ctx Context being configured.
 */
static void cwist_h3_setup_session_tickets(SSL_CTX *ssl_ctx) {
    SSL_CTX_set_session_cache_mode(ssl_ctx, SSL_SESS_CACHE_SERVER);

    pthread_once(&g_h3_ticket_key_ex_data_once, cwist_h3_ticket_key_ex_data_init);
    if (g_h3_ticket_key_ex_data_idx < 0) return;

    cwist_h3_ticket_key *key = (cwist_h3_ticket_key *)cwist_alloc(sizeof(*key));
    if (!key) return;
    if (RAND_bytes((uint8_t *)key, sizeof(*key)) != 1) {
        cwist_free(key);
        return; /* Fall back to the library-internal (per-worker) keys. */
    }
    SSL_CTX_set_ex_data(ssl_ctx, g_h3_ticket_key_ex_data_idx, key);
    SSL_CTX_set_tlsext_ticket_key_cb(ssl_ctx, cwist_h3_ticket_key_cb);
}

static void cwist_h3_free_session_ticket_key(SSL_CTX *ssl_ctx) {
    if (!ssl_ctx || g_h3_ticket_key_ex_data_idx < 0) return;
    void *key = SSL_CTX_get_ex_data(ssl_ctx, g_h3_ticket_key_ex_data_idx);
    if (key) {
        SSL_CTX_set_ex_data(ssl_ctx, g_h3_ticket_key_ex_data_idx, NULL);
        cwist_free(key);
    }
}

static int cwist_h3_ssl_ctx_init(SSL_CTX *ssl_ctx, int early_data) {
    SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ssl_ctx, TLS1_3_VERSION);
    SSL_CTX_set_default_verify_paths(ssl_ctx);
    SSL_CTX_set_alpn_select_cb(ssl_ctx, cwist_h3_alpn_select_cb, NULL);
    cwist_h3_setup_session_tickets(ssl_ctx);

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
        cwist_h3_free_session_ticket_key(ssl_ctx);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    if (cwist_tls_autoload_intermediates(ssl_ctx) < 0) {
        cwist_h3_free_session_ticket_key(ssl_ctx);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
        cwist_h3_free_session_ticket_key(ssl_ctx);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    *ctx = (cwist_http3_context *)cwist_alloc(sizeof(cwist_http3_context));
    if (!*ctx) {
        cwist_h3_free_session_ticket_key(ssl_ctx);
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
        cwist_h3_free_session_ticket_key(ssl_ctx);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }
    if (EVP_PKEY_keygen_init(pctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        cwist_h3_free_session_ticket_key(ssl_ctx);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }
    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen(pctx, &pkey) <= 0 || !pkey) {
        EVP_PKEY_CTX_free(pctx);
        cwist_h3_free_session_ticket_key(ssl_ctx);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }
    EVP_PKEY_CTX_free(pctx);

    X509 *x509 = X509_new();
    if (!x509) {
        EVP_PKEY_free(pkey);
        cwist_h3_free_session_ticket_key(ssl_ctx);
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
        cwist_h3_free_session_ticket_key(ssl_ctx);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    X509_free(x509);
    EVP_PKEY_free(pkey);

    *ctx = (cwist_http3_context *)cwist_alloc(sizeof(cwist_http3_context));
    if (!*ctx) {
        cwist_h3_free_session_ticket_key(ssl_ctx);
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

/* Graceful shutdown: send GOAWAY on every live connection (and stop
 * accepting new ones), then give the engine one chance to flush the queued
 * packets before teardown.  lsquic_engine_cooldown() marks full
 * connections going away (GOAWAY per RFC 9114 Section 5.2); the ensuing
 * lsquic_engine_destroy() closes them with CONNECTION_CLOSE/H3_NO_ERROR
 * instead of leaving clients hanging on silence. */
static void cwist_h3_engine_graceful_stop(cwist_http3_context *ctx) {
    lsquic_engine_t *engine = (lsquic_engine_t *)ctx->engine;
    if (!engine) return;
    lsquic_engine_cooldown(engine);
    lsquic_engine_process_conns(engine);
    if (ctx->udp_fd >= 0 && lsquic_engine_has_unsent_packets(engine)) {
        lsquic_engine_send_unsent_packets(engine);
    }
}

void cwist_http3_destroy_context(cwist_http3_context *ctx) {
    if (ctx) {
        if (ctx->engine) {
            cwist_h3_engine_graceful_stop(ctx);
            lsquic_engine_destroy((lsquic_engine_t *)ctx->engine);
            ctx->engine = NULL;
        }
        cwist_h3_hset_sweep(ctx);
        if (ctx->ssl_ctx) {
            cwist_h3_free_session_ticket_key(ctx->ssl_ctx);
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
    settings.es_support_push = ctx->push_enabled;
    settings.es_allow_migration = ctx->allow_migration ? ctx->allow_migration : 1;
    settings.es_max_delayed_0rtt_packets = 32;
    settings.es_datagrams = ctx->datagram_enabled;
#ifdef CWIST_WEBTRANSPORT
    if (ctx->wt_handler) {
        /* Required lsquic settings for WebTransport (see DEV_LSQUIC.md).
         * RFC 9297 Section 3.1: H3_DATAGRAM is only meaningful when the
         * QUIC DATAGRAM transport parameter is also sent, so enabling
         * WebTransport (or H3 datagrams) must force es_datagrams on. */
        settings.es_webtransport = 1;
        settings.es_http_datagrams = 1;
        settings.es_datagrams = 1;
        settings.es_max_webtransport_sessions = 1;
        settings.es_reset_stream_at = 1;
    } else {
        settings.es_http_datagrams = ctx->datagram_enabled;
    }
#endif
    settings.es_max_cfcw = 16 * 1024 * 1024;
    settings.es_max_sfcw = 8 * 1024 * 1024;
    settings.es_init_max_data = 16 * 1024 * 1024;
    settings.es_init_max_stream_data_bidi_remote = 8 * 1024 * 1024;
    settings.es_init_max_stream_data_bidi_local = 8 * 1024 * 1024;
    settings.es_init_max_stream_data_uni = 8 * 1024 * 1024;
    settings.es_init_max_streams_bidi = 256;
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

    /* ea_packets_out_ctx points at ctx->udp_fd (not the udp_fd local) so the
     * pointer stays valid for the graceful-shutdown flush in
     * cwist_http3_destroy_context(), which may run after this function's
     * frame is gone. */
    ctx->udp_fd = udp_fd;

    struct lsquic_engine_api api = {
        .ea_stream_if        = &cwist_h3_stream_if,
        .ea_stream_if_ctx    = ctx,
        .ea_packets_out      = cwist_h3_packets_out,
        .ea_packets_out_ctx  = &ctx->udp_fd,
        .ea_get_ssl_ctx      = cwist_h3_get_ssl_ctx,
        .ea_hsi_if           = &cwist_h3_hset_if,
        .ea_hsi_ctx          = ctx,
        .ea_settings         = &settings,
        .ea_alpn             = "h3",
    };

    lsquic_engine_t *engine = lsquic_engine_new(LSENG_HTTP_SERVER, &api);
    if (!engine) {
        err.error.err_i16 = -1;
        return err;
    }

    /* Diagnostic hook: CWIST_H3_DEBUG=1 routes lsquic's internal logger to
     * stderr so CONNECTION_CLOSE error codes become visible in the journal. */
    if (getenv("CWIST_H3_DEBUG")) {
        static const struct lsquic_logger_if h3_log_if = {
            .log_buf = cwist_h3_log_stderr,
        };
        lsquic_logger_init(&h3_log_if, NULL, LLTS_HHMMSSMS);
        lsquic_logger_lopt("engine=debug,conn=debug,event=debug");
    }

    ctx->engine = engine;
    ctx->handler = handler;
    ctx->user_ctx = user_ctx;
    ctx->running = 1;

    /* The receive loop relies on MSG_DONTWAIT, which some platforms lack
     * (it degrades to 0).  Guarantee non-blocking semantics at the socket
     * level instead; idempotent if the caller already set O_NONBLOCK. */
    int fl = fcntl(udp_fd, F_GETFL, 0);
    if (fl >= 0 && !(fl & O_NONBLOCK)) {
        fcntl(udp_fd, F_SETFL, fl | O_NONBLOCK);
    }

    struct sockaddr_storage local_addr;
    socklen_t local_addr_len = sizeof(local_addr);
    if (getsockname(udp_fd, (struct sockaddr *)&local_addr, &local_addr_len) != 0) {
        local_addr_len = 0;
    }

#ifdef IP_PKTINFO
    int opt_pktinfo = 1;
    setsockopt(udp_fd, IPPROTO_IP, IP_PKTINFO, &opt_pktinfo, sizeof(opt_pktinfo));
#endif
#ifdef IP_RECVTOS
    int opt_tos = 1;
    setsockopt(udp_fd, IPPROTO_IP, IP_RECVTOS, &opt_tos, sizeof(opt_tos));
#endif
#ifdef IPV6_RECVPKTINFO
    int opt_pktinfo6 = 1;
    setsockopt(udp_fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &opt_pktinfo6, sizeof(opt_pktinfo6));
#endif
#ifdef IPV6_RECVTCLASS
    int opt_tclass = 1;
    setsockopt(udp_fd, IPPROTO_IPV6, IPV6_RECVTCLASS, &opt_tclass, sizeof(opt_tclass));
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
        free(pkt_buf);
        err.error.err_i16 = -1;
        lsquic_engine_destroy(engine);
        ctx->engine = NULL;
        return err;
    }
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = udp_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, udp_fd, &ev) < 0) {
        free(pkt_buf);
        close(epoll_fd);
        err.error.err_i16 = -1;
        lsquic_engine_destroy(engine);
        ctx->engine = NULL;
        return err;
    }
#endif

    while (ctx && ctx->running && atomic_load(&g_cwist_running)) {
        int diff = 100000;
        int timeout_ms = 50;
        bool has_tick = lsquic_engine_earliest_adv_tick(engine, &diff);
        if (has_tick) {
            if (diff <= 0)
                timeout_ms = 0;
            else if (diff < 1000)
                timeout_ms = 0;
            else if (diff > 1000000)
                timeout_ms = 1000;
            else
                timeout_ms = (diff + 999) / 1000;
        }
        if (lsquic_engine_has_unsent_packets(engine)) {
            timeout_ms = 0;
        }

#ifdef __linux__
        struct epoll_event events[1];
        int pret = epoll_wait(epoll_fd, events, 1, timeout_ms);
        if (pret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pret > 0 && (events[0].events & (EPOLLERR | EPOLLHUP))) {
            fprintf(stderr, "[HTTP/3] UDP socket error, exiting loop.\n");
            break;
        }
        bool can_read = (pret > 0 && (events[0].events & EPOLLIN));
#else
        struct pollfd pfd = { .fd = udp_fd, .events = POLLIN };
        int pret = poll(&pfd, 1, timeout_ms);

        if (pret < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EBADF) {
                fprintf(stderr, "[HTTP/3] UDP socket closed, exiting loop.\n");
                break;
            }
            break;
        }

        if (pret > 0 && (pfd.revents & (POLLERR | POLLNVAL))) {
            fprintf(stderr, "[HTTP/3] UDP socket error, exiting loop.\n");
            break;
        }
        bool can_read = (pret > 0 && (pfd.revents & POLLIN));
#endif

#if defined(__linux__) && defined(_GNU_SOURCE)
#define H3_RECV_BATCH 32
        if (can_read) {
            /* Batch scratch buffers live on this (dedicated, default-stack)
             * H3 thread's stack, not in TLS: 2.1MB of .tbss here forced glibc
             * to reject every explicit pthread stack size below ~2.2MB. */
            unsigned char batch_bufs[H3_RECV_BATCH][65535];
            struct sockaddr_storage batch_peers[H3_RECV_BATCH];
            char batch_cmsgs[H3_RECV_BATCH][512];
            struct iovec batch_iovs[H3_RECV_BATCH];
            struct mmsghdr batch_msgs[H3_RECV_BATCH];

            while (1) {
                for (int b = 0; b < H3_RECV_BATCH; b++) {
                    batch_iovs[b].iov_base = batch_bufs[b];
                    batch_iovs[b].iov_len = sizeof(batch_bufs[b]);
                    memset(&batch_msgs[b], 0, sizeof(batch_msgs[b]));
                    batch_msgs[b].msg_hdr.msg_name = &batch_peers[b];
                    batch_msgs[b].msg_hdr.msg_namelen = sizeof(batch_peers[b]);
                    batch_msgs[b].msg_hdr.msg_iov = &batch_iovs[b];
                    batch_msgs[b].msg_hdr.msg_iovlen = 1;
                    batch_msgs[b].msg_hdr.msg_control = batch_cmsgs[b];
                    batch_msgs[b].msg_hdr.msg_controllen = sizeof(batch_cmsgs[b]);
                }

                int nmsgs = recvmmsg(udp_fd, batch_msgs, H3_RECV_BATCH, MSG_DONTWAIT, NULL);
                if (nmsgs <= 0) {
                    if (nmsgs < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        if (errno == ECONNREFUSED || errno == ENETUNREACH || errno == EHOSTUNREACH)
                            continue;
                        if (errno == EBADF) break;
                    }
                    break;
                }

                for (int b = 0; b < nmsgs; b++) {
                    size_t nr = (size_t)batch_msgs[b].msg_len;
                    if (nr == 0) continue;

                    struct sockaddr_storage cur_local_addr;
                    socklen_t cur_local_len = local_addr_len;
                    if (local_addr_len) memcpy(&cur_local_addr, &local_addr, local_addr_len);

                    int ecn = 0;
                    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&batch_msgs[b].msg_hdr); cmsg != NULL; cmsg = CMSG_NXTHDR(&batch_msgs[b].msg_hdr, cmsg)) {
                        if (cmsg->cmsg_level == IPPROTO_IP) {
#ifdef IP_PKTINFO
                            if (cmsg->cmsg_type == IP_PKTINFO) {
                                struct in_pktinfo *pi = (struct in_pktinfo *)CMSG_DATA(cmsg);
                                if (pi->ipi_addr.s_addr != INADDR_ANY) {
                                    struct sockaddr_in *sin = (struct sockaddr_in *)&cur_local_addr;
                                    sin->sin_family = AF_INET;
                                    sin->sin_addr = pi->ipi_addr;
                                    if (local_addr_len >= sizeof(struct sockaddr_in)) {
                                        sin->sin_port = ((struct sockaddr_in *)&local_addr)->sin_port;
                                    }
                                    cur_local_len = sizeof(struct sockaddr_in);
                                }
                            }
#endif
#ifdef IP_TOS
                            if (cmsg->cmsg_type == IP_TOS) {
                                ecn = *(int *)CMSG_DATA(cmsg) & 0x3;
                            }
#endif
                        } else if (cmsg->cmsg_level == IPPROTO_IPV6) {
#ifdef IPV6_PKTINFO
                            if (cmsg->cmsg_type == IPV6_PKTINFO) {
                                struct in6_pktinfo *pi6 = (struct in6_pktinfo *)CMSG_DATA(cmsg);
                                if (memcmp(&pi6->ipi6_addr, &in6addr_any, sizeof(struct in6_addr)) != 0) {
                                    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&cur_local_addr;
                                    sin6->sin6_family = AF_INET6;
                                    sin6->sin6_addr = pi6->ipi6_addr;
                                    if (local_addr_len >= sizeof(struct sockaddr_in6)) {
                                        sin6->sin6_port = ((struct sockaddr_in6 *)&local_addr)->sin6_port;
                                    }
                                    cur_local_len = sizeof(struct sockaddr_in6);
                                }
                            }
#endif
#ifdef IPV6_TCLASS
                            if (cmsg->cmsg_type == IPV6_TCLASS) {
                                ecn = *(int *)CMSG_DATA(cmsg) & 0x3;
                            }
#endif
                        }
                    }

                    lsquic_engine_packet_in(engine, batch_bufs[b], nr,
                                            cur_local_len ? (struct sockaddr *)&cur_local_addr : NULL,
                                            (struct sockaddr *)&batch_peers[b],
                                            ctx, ecn);
                }
            }
        }
#else
        if (can_read) {
            while (1) {
                struct sockaddr_storage peer_addr;
                socklen_t peer_addr_len = sizeof(peer_addr);
                struct sockaddr_storage cur_local_addr;
                socklen_t cur_local_len = local_addr_len;
                if (local_addr_len) memcpy(&cur_local_addr, &local_addr, local_addr_len);

                struct msghdr msg = {0};
                struct iovec iov = { pkt_buf, 65535 };
                msg.msg_name = &peer_addr;
                msg.msg_namelen = peer_addr_len;
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;

                char cmsg_buf[512];
                msg.msg_control = cmsg_buf;
                msg.msg_controllen = sizeof(cmsg_buf);

                ssize_t nr = recvmsg(udp_fd, &msg, MSG_DONTWAIT);
                if (nr <= 0) {
                    if (nr < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        if (errno == ECONNREFUSED || errno == ENETUNREACH || errno == EHOSTUNREACH) {
                            continue;
                        }
                        if (errno == EBADF) {
                            fprintf(stderr, "[HTTP/3] UDP socket closed.\n");
                            break;
                        }
                    }
                    break;
                }

                int ecn = 0;
                for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                    if (cmsg->cmsg_level == IPPROTO_IP) {
#ifdef IP_PKTINFO
                        if (cmsg->cmsg_type == IP_PKTINFO) {
                            struct in_pktinfo *pi = (struct in_pktinfo *)CMSG_DATA(cmsg);
                            if (pi->ipi_addr.s_addr != INADDR_ANY) {
                                struct sockaddr_in *sin = (struct sockaddr_in *)&cur_local_addr;
                                sin->sin_family = AF_INET;
                                sin->sin_addr = pi->ipi_addr;
                                if (local_addr_len >= sizeof(struct sockaddr_in)) {
                                    sin->sin_port = ((struct sockaddr_in *)&local_addr)->sin_port;
                                }
                                cur_local_len = sizeof(struct sockaddr_in);
                            }
                        }
#endif
#ifdef IP_TOS
                        if (cmsg->cmsg_type == IP_TOS) {
                            ecn = *(int *)CMSG_DATA(cmsg) & 0x3;
                        }
#endif
                    } else if (cmsg->cmsg_level == IPPROTO_IPV6) {
#ifdef IPV6_PKTINFO
                        if (cmsg->cmsg_type == IPV6_PKTINFO) {
                            struct in6_pktinfo *pi6 = (struct in6_pktinfo *)CMSG_DATA(cmsg);
                            if (memcmp(&pi6->ipi6_addr, &in6addr_any, sizeof(struct in6_addr)) != 0) {
                                struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&cur_local_addr;
                                sin6->sin6_family = AF_INET6;
                                sin6->sin6_addr = pi6->ipi6_addr;
                                if (local_addr_len >= sizeof(struct sockaddr_in6)) {
                                    sin6->sin6_port = ((struct sockaddr_in6 *)&local_addr)->sin6_port;
                                }
                                cur_local_len = sizeof(struct sockaddr_in6);
                            }
                        }
#endif
#ifdef IPV6_TCLASS
                        if (cmsg->cmsg_type == IPV6_TCLASS) {
                            ecn = *(int *)CMSG_DATA(cmsg) & 0x3;
                        }
#endif
                    }
                }

                lsquic_engine_packet_in(engine, pkt_buf, (size_t)nr,
                                        cur_local_len ? (struct sockaddr *)&cur_local_addr : NULL,
                                        (struct sockaddr *)&peer_addr,
                                        ctx, ecn);
            }
        }
#endif

        lsquic_engine_process_conns(engine);
        if (lsquic_engine_has_unsent_packets(engine)) {
            lsquic_engine_send_unsent_packets(engine);
        }
    }

#ifdef __linux__
    if (epoll_fd >= 0) close(epoll_fd);
#endif

    free(pkt_buf);
    if (ctx && ctx->engine) {
        cwist_h3_engine_graceful_stop(ctx);
        lsquic_engine_destroy((lsquic_engine_t *)ctx->engine);
        ctx->engine = NULL;
        cwist_h3_hset_sweep(ctx);
    }
    err.error.err_i16 = 0;
    return err;
}

/* ------------------------------------------------------------------ */
/* Push, Priority, and 0-RTT APIs                                     */
/* ------------------------------------------------------------------ */

void cwist_http3_set_push_enabled(cwist_http3_context *ctx, int enabled) {
    if (ctx) ctx->push_enabled = enabled;
}

void cwist_http3_set_early_data(cwist_http3_context *ctx, bool enabled) {
    if (!ctx || !ctx->ssl_ctx) return;
    ctx->early_data_enabled = enabled ? 1 : 0;
    SSL_CTX_set_early_data_enabled(ctx->ssl_ctx, enabled ? 1 : 0);
    if (enabled) {
        /* Replay protection is opt-out: restrict 0-RTT requests to
         * idempotent methods unless the application overrides the guard. */
        ctx->early_data_guard = 1;
    }
}

void cwist_http3_set_early_data_guard(cwist_http3_context *ctx, int enabled) {
    if (ctx) ctx->early_data_guard = enabled ? 1 : 0;
}

/* RFC 9110: string-based variant used by tests and external checks. */
int cwist_http3_method_is_idempotent(const char *method_str) {
    if (!method_str) return 0;
    /* TRACE is absent from cwist_http_method_t; match it by name. */
    if (strcasecmp(method_str, "TRACE") == 0) return 1;
    return h3_method_is_idempotent(cwist_http_string_to_method(method_str)) ? 1 : 0;
}

int cwist_http3_push_resource(cwist_http_request *req,
                              const char *path,
                              const char *content_type) {
    if (!req || !req->private_data || !path) return -1;
    lsquic_stream_t *stream = (lsquic_stream_t *)req->private_data;
    lsquic_conn_t *conn = lsquic_stream_conn(stream);
    if (!conn || !lsquic_conn_is_push_enabled(conn)) return -1;
    (void)content_type; /* response field; not valid in a push request set */

    /* Build push headers (a request header set needs :method, :scheme,
     * :authority, :path; content-type is a response field and is omitted) */
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

    /* :scheme = https (HTTP/3 is always over QUIC/TLS) */
    const char *scheme = "https";
    size_t sclen = strlen(scheme);
    if (hbuf_off + 7 + 2 + sclen <= sizeof(hbuf)) {
        memcpy(hbuf + hbuf_off, ":scheme", 7);
        memcpy(hbuf + hbuf_off + 9, scheme, sclen);
        lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off,
                                   0, 7, 9, sclen);
        hbuf_off += 9 + sclen;
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

    lsquic_http_headers_t headers = {
        .count = (unsigned)hdr_count,
        .headers = headers_arr,
    };

    return lsquic_conn_push_stream(conn, NULL, stream, &headers);
}

int cwist_http3_set_stream_priority(cwist_http_request *req, unsigned priority) {
    /* Refused on purpose: lsquic_stream_set_priority() on a request stream
     * emits a PRIORITY_UPDATE frame on the control stream, which violates
     * RFC 9218 (PRIORITY_UPDATE may only reference client-opened streams).
     * Strict stacks (Firefox/neqo) then kill the connection with
     * H3_FRAME_UNEXPECTED. Use the RFC 9218 `Priority` header instead.
     * See the deprecation note in include/cwist/net/http/http3.h. */
    (void)req; (void)priority;
    CWIST_LOG_WARN("[HTTP/3] cwist_http3_set_stream_priority() is deprecated "
                   "and refused (RFC 9218 violation); use the Priority header");
    return -1;
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
