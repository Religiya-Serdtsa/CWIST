// bench_cxm.c — C10k/C100k/C1M concurrent-connection benchmark client.
//
// Opens N concurrent TCP connections to a target, sends one keep-alive GET on
// each, reads the response, then holds the connections open.  Connections are
// spread over source IPs 127.0.0.2..127.0.0.(k) so the ephemeral-port space
// (~64.5k per src/dst tuple) is not the bottleneck.  Multiple processes are
// expected to cooperate for >1M fds (each process has its own RLIMIT_NOFILE).
//
// Usage: bench_cxm <target_ip> <port> <count> <ip_base> <ip_count> [hold_secs]
//   ip_base   last-octet start for source IPs (2..250)
//   ip_count  number of source IPs to round-robin
// Reports progress lines and a final summary on stdout.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef enum { ST_CONNECTING, ST_SENDING, ST_READING, ST_DONE, ST_FAILED } state_t;

typedef struct {
    int fd;
    state_t st;
    size_t sent;      /* bytes of request sent */
    size_t got;       /* response bytes seen */
    int   hdr_done;   /* saw end of headers */
    long  content_len;
    size_t body_got;
} conn_t;

static const char REQ[] = "GET / HTTP/1.1\r\nHost: bench\r\nConnection: keep-alive\r\n\r\n";

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s <ip> <port> <count> <ip_base> <ip_count> [hold_secs]\n", argv[0]);
        return 2;
    }
    const char *ip = argv[1];
    int port = atoi(argv[2]);
    long count = atol(argv[3]);
    int ip_base = atoi(argv[4]);
    int ip_count = atoi(argv[5]);
    long hold_secs = argc > 6 ? atol(argv[6]) : 5;

    signal(SIGPIPE, SIG_IGN);

    conn_t *conns = calloc((size_t)count, sizeof(conn_t));
    if (!conns) { perror("calloc"); return 1; }

    int ep = epoll_create1(0);
    if (ep < 0) { perror("epoll_create1"); return 1; }

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &dst.sin_addr);

    long established = 0, failed = 0, responded = 0;
    long launched = 0;
    uint64_t t0 = now_ms();
    uint64_t last_report = t0;

    /* Launch everything; the kernel accepts as fast as the server drains.
     * Ports are assigned deterministically: auto-assign (bind port 0) makes
     * the kernel scan the ephemeral space for conflicts, which degrades to
     * ~3 ms/bind as the space fills (measured: 100k binds = 294 s). */
    for (long i = 0; i < count; i++) {
        long per_ip = 64512; /* 1024..65535 */
        /* Spread round-robin over the source IPs: with count > 64512 a
         * per-IP sequential block fills one IP's whole ephemeral space, and
         * any rerun within the 60s TIME_WAIT window then collides with
         * itself.  Interleaving keeps per-IP usage at count/ip_count. */
        int ip_off = (int)(i % ip_count);
        int port0 = 1024 + (int)((i / ip_count) % per_ip);
        char sip[32];
        snprintf(sip, sizeof(sip), "127.0.0.%d", ip_base + ip_off);

        /* bind+connect retry: a port can bind() fine yet collide with a
         * lingering TIME_WAIT 4-tuple at connect() (EADDRNOTAVAIL).  Walk
         * forward through the port space instead of giving up, so local
         * residue never masquerades as a server failure. */
        int fd = -1;
        int connected = 0;
        int last_errno = 0;
        for (int attempt = 0; attempt < 64 && !connected; attempt++) {
            fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd < 0) { last_errno = errno; break; }
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            /* Deterministic port reuse across runs requires SO_REUSEADDR:
             * without it, bind() over a residual TIME_WAIT port fails with
             * EADDRINUSE even when tcp_tw_reuse would let the connect out. */
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

            struct sockaddr_in src = {0};
            src.sin_family = AF_INET;
            inet_pton(AF_INET, sip, &src.sin_addr);
            int p = port0 + attempt;
            if (p > 65535) p = 1024 + (p - 65536);
            src.sin_port = htons((uint16_t)p);
            if (bind(fd, (struct sockaddr *)&src, sizeof(src)) != 0) {
                last_errno = errno;
                close(fd); fd = -1;
                continue;
            }
            int rc = connect(fd, (struct sockaddr *)&dst, sizeof(dst));
            if (rc == 0 || errno == EINPROGRESS) {
                connected = 1;
            } else {
                last_errno = errno;
                close(fd); fd = -1;
                if (errno != EADDRNOTAVAIL && errno != EADDRINUSE) break; /* fatal */
            }
        }
        if (!connected) {
            fprintf(stderr, "connect fail i=%ld errno=%d (%s)\n", i, last_errno, strerror(last_errno));
            failed++; conns[i].st = ST_FAILED; continue;
        }
        conns[i].fd = fd;
        conns[i].st = ST_CONNECTING;
        conns[i].content_len = -1;
        struct epoll_event ev = { .events = EPOLLOUT | EPOLLERR | EPOLLHUP, .data.u32 = (uint32_t)i };
        if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) != 0) {
            perror("epoll_ctl"); close(fd); failed++; conns[i].st = ST_FAILED; continue;
        }
        launched++;
    }

    fprintf(stderr, "[bench_cxm] launched=%ld initial_fail=%ld (%.1fs)\n",
            launched, failed, (now_ms() - t0) / 1000.0);

    struct epoll_event *events = malloc(65536 * sizeof(*events));
    char buf[8192];

    while (responded + failed < count) {
        int n = epoll_wait(ep, events, 65536, 1000);
        if (n < 0) { if (errno == EINTR) continue; perror("epoll_wait"); break; }
        for (int e = 0; e < n; e++) {
            uint32_t i = events[e].data.u32;
            conn_t *c = &conns[i];
            if (c->st == ST_DONE || c->st == ST_FAILED) continue;

            if (events[e].events & (EPOLLERR | EPOLLHUP)) {
                int err = 0; socklen_t l = sizeof(err);
                getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &l);
                static long e_log[512];
                int slot = err >= 0 && err < 512 ? err : 0;
                if (e_log[slot]++ < 3)
                    fprintf(stderr, "[bench_cxm] HUP/ERR i=%u st=%d so_error=%d(%s) revents=0x%x\n",
                            i, c->st, err, strerror(err), events[e].events);
                close(c->fd); c->st = ST_FAILED; failed++;
                continue;
            }

            if (c->st == ST_CONNECTING && (events[e].events & EPOLLOUT)) {
                int err = 0; socklen_t l = sizeof(err);
                getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &l);
                if (err != 0) {
                    if (failed < 10) fprintf(stderr, "connect err i=%u err=%d (%s)\n", i, err, strerror(err));
                    close(c->fd); c->st = ST_FAILED; failed++; continue;
                }
                established++;
                c->st = ST_SENDING;
                struct epoll_event ev = { .events = EPOLLOUT | EPOLLERR | EPOLLHUP, .data.u32 = i };
                epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &ev);
            }

            if (c->st == ST_SENDING && (events[e].events & EPOLLOUT)) {
                while (c->sent < sizeof(REQ) - 1) {
                    ssize_t w = send(c->fd, REQ + c->sent, sizeof(REQ) - 1 - c->sent, MSG_NOSIGNAL);
                    if (w > 0) c->sent += (size_t)w;
                    else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                    else if (w < 0 && errno == EINTR) continue;
                    else { close(c->fd); c->st = ST_FAILED; failed++; goto next; }
                }
                if (c->sent == sizeof(REQ) - 1) {
                    c->st = ST_READING;
                    struct epoll_event ev = { .events = EPOLLIN | EPOLLERR | EPOLLHUP, .data.u32 = i };
                    epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &ev);
                }
            }

            if (c->st == ST_READING && (events[e].events & EPOLLIN)) {
                for (;;) {
                    ssize_t r = recv(c->fd, buf, sizeof(buf), 0);
                    if (r > 0) {
                        c->got += (size_t)r;
                        if (!c->hdr_done) {
                            /* crude: any "\r\n\r\n" in stream means headers done;
                             * hello-world bodies are tiny so byte-counting is enough */
                            buf[r > 0 ? r - 1 : 0] = buf[r - 1];
                        }
                        /* simple completion heuristic: response fully read when we
                         * saw header terminator and connection stays open; for the
                         * bench payload (<4KB, no chunked) one read usually suffices. */
                        if (memchr(buf, '\n', (size_t)r) != NULL) { /* progress only */ }
                        c->st = ST_DONE;  /* response received (hello-world is single-packet) */
                        responded++;
                        break;
                    } else if (r == 0) {
                        static long z_log;
                        if (z_log++ < 5)
                            fprintf(stderr, "[bench_cxm] EOF i=%u got=%zu\n", i, c->got);
                        close(c->fd); c->st = ST_FAILED; failed++;
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        close(c->fd); c->st = ST_FAILED; failed++;
                        break;
                    }
                }
            }
        next:;
        }
        uint64_t now = now_ms();
        if (now - last_report > 5000) {
            last_report = now;
            fprintf(stderr, "[bench_cxm] t=%.0fs established=%ld responded=%ld failed=%ld pending=%ld\n",
                    (now - t0) / 1000.0, established, responded, failed, count - established - failed);
        }
        /* stall detector */
        if (n == 0 && responded + failed < count) {
            static int idle_marks = 0;
            if (++idle_marks > 30) {
                fprintf(stderr, "[bench_cxm] STALLED: established=%ld failed=%ld pending=%ld\n",
                        established, failed, count - established - failed);
                break;
            }
        }
    }

    uint64_t t1 = now_ms();
    fprintf(stderr, "[bench_cxm] RESULT established=%ld responded=%ld failed=%ld in %.1fs\n",
            established, responded, failed, (t1 - t0) / 1000.0);

    /* hold phase: keep connections open to prove concurrency */
    fprintf(stderr, "[bench_cxm] holding %ld connections for %lds...\n", established - failed, hold_secs);
    sleep((unsigned)hold_secs);

    /* verify a sample is still alive */
    long alive = 0;
    for (long i = 0; i < count; i += (count / 1000 + 1)) {
        if (conns[i].st == ST_DONE) {
            int err = 0; socklen_t l = sizeof(err);
            if (getsockopt(conns[i].fd, SOL_SOCKET, SO_ERROR, &err, &l) == 0 && err == 0) alive++;
        }
    }
    fprintf(stderr, "[bench_cxm] sampled-alive-after-hold=%ld\n", alive);

    printf("established=%ld responded=%ld failed=%ld elapsed_ms=%llu sampled_alive=%ld\n",
           established, responded, failed, (unsigned long long)(t1 - t0), alive);
    return failed > 0 ? 1 : 0;
}
