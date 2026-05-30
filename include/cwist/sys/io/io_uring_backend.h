/**
 * @file io_uring_backend.h
 * @brief Low-latency io_uring backend for CWIST event loop integration.
 *
 * Directly uses the Linux io_uring UAPI (via syscall) to avoid liburing
 * dependency while retaining full control over SQ/CQ rings, fixed buffers,
 * and multi-shot operations.
 *
 * Design goals:
 *  - Lock-free submission when possible (SQPOLL opt-in).
 *  - Fixed buffers for hot-path networking to reduce TLB pressure.
 *  - Unified TCP/UDP sendmsg/recvmsg paths for HTTP/1–3.
 */

#ifndef __CWIST_IO_URING_BACKEND_H__
#define __CWIST_IO_URING_BACKEND_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct cwist_core_stream;
struct cwist_uring_buf_ring;

/** @brief Opaque handle for the io_uring backend instance. */
typedef struct cwist_uring_backend cwist_uring_backend_t;

/** @brief Completion callback signature. */
typedef void (*cwist_uring_cqe_cb)(struct cwist_core_stream *stream,
                                    int32_t res,
                                    uint32_t flags,
                                    void *user_data);

/** @brief Per-stream I/O context registered with the backend. */
typedef struct cwist_core_stream {
    int                     fd;
    uint64_t                stream_id;   ///< Unique identifier (H2/H3 stream id or counter).
    cwist_uring_cqe_cb      recv_cb;     ///< Callback for read completions.
    cwist_uring_cqe_cb      send_cb;     ///< Callback for write completions.
    void                   *user_data;   ///< Handler context (e.g., cwist_http_req_t *).
    struct cwist_core_stream *next;      ///< Intrusive list for stream tracking.
    struct cwist_core_stream *prev;
    uint32_t                protocol;    ///< 1=H1, 2=H2, 3=H3/QUIC.
    uint32_t                flags;       ///< Internal state flags.

    /* --- 2-Phase Demolition & Generation ID --- */
    atomic_int              pending_io_count; ///< Number of SQEs in-flight in kernel.
    uint32_t                generation;       ///< Generation ticket to detect zombie CQEs.
    uint32_t                index;            ///< Index in the backend's stream pool.
    bool                    is_dead;          ///< Marked for destruction.
    uint64_t                deadline_ts;      ///< Absolute time (ms) for forced GC.
} cwist_core_stream_t;

/** @brief Fixed buffer region used with IORING_OP_PROVIDE_BUFFERS. */
typedef struct cwist_uring_buf_pool {
    uint8_t *base;
    size_t   buf_size;
    size_t   buf_count;
    int     *bid_map;       ///< bid -> availability (0=free, 1=in-flight).
    size_t   free_count;
} cwist_uring_buf_pool_t;

/** @brief Configuration for the io_uring backend. */
typedef struct cwist_uring_config {
    uint32_t sq_entries;      ///< Submission queue depth (default 4096).
    uint32_t cq_entries;      ///< Completion queue depth (default 8192).
    uint32_t max_streams;     ///< Maximum concurrent sessions (default 131072).
    bool     sqpoll;          ///< Enable SQPOLL kernel thread (privileged).
    bool     iopoll;          ///< Enable IOPOLL (busy-wait, privileged).
    bool     use_fixed_buf;   ///< Register fixed buffers at init.
    size_t   fixed_buf_size;  ///< Size of each fixed buffer (default 16 KiB).
    size_t   fixed_buf_count; ///< Number of fixed buffers (default 1024).
} cwist_uring_config_t;

/**
 * @brief Default configuration initializer.
 */
#define CWIST_URING_CONFIG_DEFAULT { \
    .sq_entries = 4096, \
    .cq_entries = 8192, \
    .max_streams = 131072, \
    .sqpoll = false, \
    .iopoll = false, \
    .use_fixed_buf = true, \
    .fixed_buf_size = 16 * 1024, \
    .fixed_buf_count = 1024 \
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/**
 * @brief Create an io_uring backend with the given configuration.
 * @param cfg Configuration, or NULL for defaults.
 * @return Backend handle, or NULL on failure.
 */
cwist_uring_backend_t *cwist_uring_backend_create(const cwist_uring_config_t *cfg);

/**
 * @brief Tear down the backend and release all kernel/user resources.
 */
void cwist_uring_backend_destroy(cwist_uring_backend_t *be);

/* -------------------------------------------------------------------------
 * Stream registration
 * ---------------------------------------------------------------------- */

/**
 * @brief Register a socket fd as a core stream with the backend.
 *
 * The backend does not take ownership of @p fd; the caller must close it
 * after unregistering.
 *
 * @param be     Backend instance.
 * @param fd     Socket file descriptor.
 * @param proto  Protocol identifier (1=H1, 2=H2, 3=H3).
 * @return New stream object, or NULL on failure.
 */
cwist_core_stream_t *cwist_uring_stream_register(cwist_uring_backend_t *be,
                                                   int fd,
                                                   uint32_t proto);

/**
 * @brief Remove a stream from the backend and free its metadata.
 */
void cwist_uring_stream_unregister(cwist_uring_backend_t *be,
                                    cwist_core_stream_t *stream);

/* -------------------------------------------------------------------------
 * Asynchronous I/O submission
 * ---------------------------------------------------------------------- */

/**
 * @brief Queue an async recvmsg on the given stream.
 *
 * On completion the stream's recv_cb is invoked with the number of bytes
 * read (or negative errno) and CQE flags.
 *
 * @param be     Backend instance.
 * @param stream Target stream.
 * @param msg    msghdr describing buffers (ignored when fixed buffers are used).
 * @return true when the SQE was successfully queued.
 */
bool cwist_uring_submit_recvmsg(cwist_uring_backend_t *be,
                                 cwist_core_stream_t *stream,
                                 struct msghdr *msg);

/**
 * @brief Queue an async sendmsg on the given stream.
 * @param be     Backend instance.
 * @param stream Target stream.
 * @param msg    msghdr describing the outgoing data.
 * @return true when the SQE was successfully queued.
 */
bool cwist_uring_submit_sendmsg(cwist_uring_backend_t *be,
                                 cwist_core_stream_t *stream,
                                 struct msghdr *msg);

/**
 * @brief Queue an async read on the given stream (file I/O path).
 */
bool cwist_uring_submit_read(cwist_uring_backend_t *be,
                              cwist_core_stream_t *stream,
                              void *buf,
                              size_t len,
                              off_t offset);

/**
 * @brief Queue an async write on the given stream (file I/O path).
 */
bool cwist_uring_submit_write(cwist_uring_backend_t *be,
                               cwist_core_stream_t *stream,
                               const void *buf,
                               size_t len,
                               off_t offset);

/**
 * @brief Queue an async splice operation (pipe-to-socket zero-copy).
 */
bool cwist_uring_submit_splice(cwist_uring_backend_t *be,
                                cwist_core_stream_t *stream_in,
                                cwist_core_stream_t *stream_out,
                                size_t len);

/**
 * @brief Queue an async NOP operation (useful for testing or signaling).
 */
bool cwist_uring_submit_nop(cwist_uring_backend_t *be,
                             cwist_core_stream_t *stream);

/**
 * @brief Queue an async close of a file descriptor via io_uring.
 */
bool cwist_uring_submit_close(cwist_uring_backend_t *be, int fd);

/* -------------------------------------------------------------------------
 * Event loop
 * ---------------------------------------------------------------------- */

/**
 * @brief Submit all pending SQEs to the kernel (io_uring_enter).
 * @return Number of submitted SQEs, or negative errno.
 */
int cwist_uring_flush_sq(cwist_uring_backend_t *be);

/**
 * @brief Wait for and process at least @p min_wait completions.
 *
 * Blocks until completions are available (or timeout).  For each CQE the
 * associated stream callback is invoked.
 *
 * @param be      Backend instance.
 * @param min_wait Minimum number of CQEs to wait for (0 = don't block).
 * @param timeout_ms Maximum milliseconds to block, or -1 for infinite.
 * @return Number of CQEs processed, or negative errno.
 */
int cwist_uring_poll_cq(cwist_uring_backend_t *be,
                         unsigned min_wait,
                         int timeout_ms);

/**
 * @brief Run the io_uring event loop until stopped.
 *
 * Repeatedly flushes the submission queue and polls the completion queue.
 * This is the primary integration point for CWIST's networking core.
 *
 * @param be Backend instance.
 */
void cwist_uring_backend_run(cwist_uring_backend_t *be);

/**
 * @brief Request graceful shutdown of the event loop.
 */
void cwist_uring_backend_stop(cwist_uring_backend_t *be);

/* -------------------------------------------------------------------------
 * Fixed buffer management
 * ---------------------------------------------------------------------- */

/**
 * @brief Acquire a fixed buffer from the pool.
 * @param be   Backend instance.
 * @param bid  [out] Buffer ID for use in SQE flags.
 * @param ptr  [out] Pointer to the buffer base.
 * @return true on success.
 */
bool cwist_uring_buf_acquire(cwist_uring_backend_t *be, uint16_t *bid, void **ptr);

/**
 * @brief Release a fixed buffer back to the pool.
 */
void cwist_uring_buf_release(cwist_uring_backend_t *be, uint16_t bid);

/**
 * @brief Query metrics: number of active SQEs waiting for kernel pickup.
 */
uint32_t cwist_uring_pending_sqes(const cwist_uring_backend_t *be);

/**
 * @brief Query metrics: number of in-flight fixed buffers.
 */
uint32_t cwist_uring_active_buffers(const cwist_uring_backend_t *be);

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_IO_URING_BACKEND_H__ */
