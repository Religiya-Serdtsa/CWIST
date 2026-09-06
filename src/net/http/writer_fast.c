#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cwist/net/http/writer_fast.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>

cwist_write_status_t cwist_http_send_speculative(int fd, const void *buf, size_t len, size_t *sent_out) {
    if (!buf || len == 0) {
        if (sent_out) *sent_out = 0;
        return CWIST_WRITE_DONE;
    }

    int flags = MSG_DONTWAIT;
#if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
#endif

    ssize_t n;
    do {
        n = send(fd, buf, len, flags);
    } while (n < 0 && errno == EINTR);

    if (n >= 0) {
        if (sent_out) *sent_out = (size_t)n;
        if ((size_t)n == len) {
            return CWIST_WRITE_DONE;
        }
        return CWIST_WRITE_PENDING;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (sent_out) *sent_out = 0;
        return CWIST_WRITE_PENDING;
    }

    return CWIST_WRITE_ERR;
}

cwist_write_status_t cwist_http_sendmsg_speculative(int fd, struct iovec *iov, int iovcnt, int flags, size_t *total_sent) {
    if (!iov || iovcnt <= 0) {
        if (total_sent) *total_sent = 0;
        return CWIST_WRITE_DONE;
    }

    int send_flags = flags | MSG_DONTWAIT;
#if defined(MSG_NOSIGNAL)
    send_flags |= MSG_NOSIGNAL;
#endif

    struct msghdr msg = {
        .msg_iov = iov,
        .msg_iovlen = (size_t)iovcnt
    };

    ssize_t n;
    do {
        n = sendmsg(fd, &msg, send_flags);
    } while (n < 0 && errno == EINTR);

    if (n >= 0) {
        if (total_sent) *total_sent = (size_t)n;

        /* Calculate total expected payload length across all iovec items */
        size_t expected = 0;
        for (int i = 0; i < iovcnt; ++i) {
            expected += iov[i].iov_len;
        }

        if ((size_t)n >= expected) {
            return CWIST_WRITE_DONE;
        }
        return CWIST_WRITE_PENDING;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (total_sent) *total_sent = 0;
        return CWIST_WRITE_PENDING;
    }

    return CWIST_WRITE_ERR;
}

uint32_t cwist_sched_p2c_select_worker(const _Atomic uint32_t *worker_loads, uint32_t num_workers) {
    if (num_workers <= 1) return 0;

    static _Thread_local uint32_t seed = 0x9e3779b9;
    if (__builtin_expect(seed == 0, 0)) {
        seed = (uint32_t)(uintptr_t)&seed;
    }

    /* Fast xorshift32 PRNG */
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    uint32_t w1 = seed % num_workers;

    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    uint32_t w2 = seed % num_workers;

    if (w1 == w2) {
        w2 = (w1 + 1) % num_workers;
    }

    uint32_t load1 = atomic_load_explicit(&worker_loads[w1], memory_order_relaxed);
    uint32_t load2 = atomic_load_explicit(&worker_loads[w2], memory_order_relaxed);

    return (load1 <= load2) ? w1 : w2;
}

uint32_t cwist_fast_monotonic_sec(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC_COARSE)
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint32_t)ts.tv_sec;
}
