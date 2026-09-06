/**
 * @file writer_fast.h
 * @brief Zero-latency speculative fast-path writer and P2C worker scheduler.
 */

#ifndef CWIST_NET_HTTP_WRITER_FAST_H
#define CWIST_NET_HTTP_WRITER_FAST_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CWIST_WRITE_DONE = 0,    /**< Complete buffer was written on the first pass (0 latency overhead). */
    CWIST_WRITE_PENDING = 1, /**< Socket buffer saturated (EAGAIN/EWOULDBLOCK) - hand off to reactor. */
    CWIST_WRITE_ERR = -1     /**< Fatal socket error or connection reset. */
} cwist_write_status_t;

/**
 * @brief Speculative non-blocking direct sender.
 *
 * Attempts an immediate non-blocking send without context switching or event loop queuing.
 * Fast clients finish synchronously (0 ms jitter). Slow clients immediately return
 * CWIST_WRITE_PENDING instead of blocking worker threads in a synchronous poll() loop.
 *
 * @param fd Target client socket file descriptor.
 * @param buf Pointer to the contiguous buffer to send.
 * @param len Total length of the buffer in bytes.
 * @param sent_out Optional pointer to store the number of bytes successfully sent.
 * @return cwist_write_status_t status indicating completion, pending state, or fatal error.
 */
cwist_write_status_t cwist_http_send_speculative(int fd, const void *buf, size_t len, size_t *sent_out);

/**
 * @brief Speculative scatter/gather sendmsg without blocking poll().
 *
 * Attempts an immediate sendmsg with MSG_DONTWAIT. If the socket cannot accept all data,
 * returns CWIST_WRITE_PENDING or advances iov cursor without blocking the reactor.
 */
cwist_write_status_t cwist_http_sendmsg_speculative(int fd, struct iovec *iov, int iovcnt, int flags, size_t *total_sent);

/**
 * @brief Select worker using Power of Two Random Choices (P2C).
 *
 * Samples two random worker slots and assigns the connection to the one with lower active count.
 * Avoids cache bouncing caused by reactive work-stealing while providing O(1) lockless balance.
 *
 * @param worker_loads Array of atomic load counters for each worker.
 * @param num_workers Total number of active worker threads/processes.
 * @return Index of the selected worker.
 */
uint32_t cwist_sched_p2c_select_worker(const _Atomic uint32_t *worker_loads, uint32_t num_workers);

/**
 * @brief Fast monotonic clock in seconds (vDSO coarse where available).
 *
 * Uses CLOCK_MONOTONIC_COARSE on Linux (1-4ns vDSO read) and falls back
 * to CLOCK_MONOTONIC on BSD/macOS.
 */
uint32_t cwist_fast_monotonic_sec(void);

#ifdef __cplusplus
}
#endif

#endif /* CWIST_NET_HTTP_WRITER_FAST_H */
