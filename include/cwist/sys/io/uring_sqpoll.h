/**
 * @file uring_sqpoll.h
 * @brief Zero-syscall I/O engine powered by Linux io_uring SQPOLL.
 */

#ifndef CWIST_SYS_IO_URING_SQPOLL_H
#define CWIST_SYS_IO_URING_SQPOLL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(__linux__)
#include <linux/io_uring.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int ring_fd;
    struct io_uring_sqe *sqes;
    _Atomic uint32_t *sq_head;
    _Atomic uint32_t *sq_tail;
    uint32_t *sq_ring_mask;
    uint32_t *sq_array;
    uint32_t entries;
    size_t sq_ring_sz;
    size_t sqes_sz;
    bool active;
} cwist_sqpoll_ring_t;

/**
 * @brief Initialize an io_uring SQPOLL ring instance.
 *
 * Spawns an in-kernel submission poller thread that continuously processes
 * submitted SQEs directly from user memory without entering syscall traps.
 *
 * @param r Ring structure to initialize.
 * @param entries Queue capacity (power of two).
 * @param sq_thread_idle_ms Idle timeout in ms before the kernel thread sleeps.
 * @return 0 on success, -1 on failure (e.g. insufficient privileges or kernel support).
 */
int cwist_io_uring_init_sqpoll(cwist_sqpoll_ring_t *r, unsigned entries, unsigned sq_thread_idle_ms);

/**
 * @brief Enqueue a SEND operation into SQPOLL ring.
 *
 * Zero-syscall memory-only submission.
 *
 * @param r Initialized SQPOLL ring.
 * @param fd Target socket descriptor.
 * @param buf Contiguous data buffer pointer.
 * @param len Data length in bytes.
 * @param user_data User tracking tag.
 * @return true if submitted to the ring, false if queue is saturated.
 */
bool cwist_io_uring_sqpoll_send(cwist_sqpoll_ring_t *r, int fd, const void *buf, size_t len, uint64_t user_data);

/**
 * @brief Destroy SQPOLL ring and unmap shared ring memory.
 *
 * @param r Ring structure to clean up.
 */
void cwist_io_uring_destroy_sqpoll(cwist_sqpoll_ring_t *r);

#ifdef __cplusplus
}
#endif

#endif /* __linux__ */

#endif /* CWIST_SYS_IO_URING_SQPOLL_H */
