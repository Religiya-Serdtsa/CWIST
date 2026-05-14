/**
 * @file io_uring_backend.c
 * @brief Direct io_uring syscall backend for CWIST (no liburing dependency).
 *
 * Uses the Linux UAPI headers directly to set up the ring, manage fixed
 * buffers, and drive SENDMSG/RECVMSG for multi-protocol networking.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cwist/sys/io/io_uring_backend.h>
#include <cwist/core/mem/alloc.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <linux/io_uring.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Direct syscall wrappers
 * ---------------------------------------------------------------------- */

static inline int sys_io_uring_setup(unsigned entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static inline int sys_io_uring_enter(int ring_fd,
                                      unsigned to_submit,
                                      unsigned min_complete,
                                      unsigned flags,
                                      sigset_t *sig) {
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit,
                        min_complete, flags, sig);
}

static inline int sys_io_uring_register(int ring_fd,
                                         unsigned opcode,
                                         const void *arg,
                                         unsigned nr_args) {
    return (int)syscall(__NR_io_uring_register, ring_fd, opcode, arg, nr_args);
}

/* -------------------------------------------------------------------------
 * Ring layout helpers
 * ---------------------------------------------------------------------- */

struct cwist_uring_sq_ring {
    uint32_t *head;
    uint32_t *tail;
    uint32_t *ring_mask;
    uint32_t *ring_entries;
    uint32_t *flags;
    uint32_t *dropped;
    uint32_t *array;
    struct io_uring_sqe *sqes;
    size_t sqe_sz;
    size_t ring_sz;
};

struct cwist_uring_cq_ring {
    uint32_t *head;
    uint32_t *tail;
    uint32_t *ring_mask;
    uint32_t *ring_entries;
    struct io_uring_cqe *cqes;
    size_t ring_sz;
};

/* -------------------------------------------------------------------------
 * Backend state
 * ---------------------------------------------------------------------- */

struct cwist_uring_backend {
    int ring_fd;
    struct cwist_uring_sq_ring sq;
    struct cwist_uring_cq_ring cq;
    struct io_uring_params params;
    cwist_uring_config_t cfg;

    cwist_core_stream_t *stream_list;   /**< Active stream doubly-linked list. */
    atomic_size_t        stream_count;
    pthread_mutex_t      stream_lock;

    cwist_uring_buf_pool_t buf_pool;
    atomic_uint          pending_sqes;  /**< SQEs not yet flushed. */
    atomic_bool          stopped;
    atomic_bool          running;
};

/* -------------------------------------------------------------------------
 * Memory-map helpers
 * ---------------------------------------------------------------------- */

static void *mmap_ring(int fd, size_t sz, off_t off) {
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, off);
    return (p == MAP_FAILED) ? NULL : p;
}

static size_t sq_ring_size(struct io_uring_params *p) {
    return p->sq_off.array + p->sq_entries * sizeof(uint32_t);
}

static size_t cq_ring_size(struct io_uring_params *p) {
    return p->cq_off.cqes + p->cq_entries * sizeof(struct io_uring_cqe);
}

/* -------------------------------------------------------------------------
 * Backend lifecycle
 * ---------------------------------------------------------------------- */

cwist_uring_backend_t *cwist_uring_backend_create(const cwist_uring_config_t *cfg_in) {
    cwist_uring_backend_t *be = cwist_alloc(sizeof(*be));
    if (!be) return NULL;
    memset(be, 0, sizeof(*be));

    be->cfg = cfg_in ? *cfg_in : (cwist_uring_config_t)CWIST_URING_CONFIG_DEFAULT;
    atomic_init(&be->pending_sqes, 0);
    atomic_init(&be->stopped, false);
    atomic_init(&be->running, false);
    pthread_mutex_init(&be->stream_lock, NULL);

    memset(&be->params, 0, sizeof(be->params));
    if (be->cfg.sqpoll) {
        be->params.flags |= IORING_SETUP_SQPOLL;
        be->params.sq_thread_idle = 2000;
    }
    if (be->cfg.iopoll) {
        be->params.flags |= IORING_SETUP_IOPOLL;
    }

    int fd = sys_io_uring_setup(be->cfg.sq_entries, &be->params);
    if (fd < 0) {
        fprintf(stderr, "[io_uring] setup failed: %s\n", strerror(-fd));
        cwist_free(be);
        return NULL;
    }
    be->ring_fd = fd;

    /* Map SQ ring */
    size_t sq_sz = sq_ring_size(&be->params);
    void *sq_ptr = mmap_ring(fd, sq_sz, IORING_OFF_SQ_RING);
    if (!sq_ptr) goto fail;
    be->sq.head         = (uint32_t *)((char *)sq_ptr + be->params.sq_off.head);
    be->sq.tail         = (uint32_t *)((char *)sq_ptr + be->params.sq_off.tail);
    be->sq.ring_mask    = (uint32_t *)((char *)sq_ptr + be->params.sq_off.ring_mask);
    be->sq.ring_entries = (uint32_t *)((char *)sq_ptr + be->params.sq_off.ring_entries);
    be->sq.flags        = (uint32_t *)((char *)sq_ptr + be->params.sq_off.flags);
    be->sq.dropped      = (uint32_t *)((char *)sq_ptr + be->params.sq_off.dropped);
    be->sq.array        = (uint32_t *)((char *)sq_ptr + be->params.sq_off.array);
    be->sq.ring_sz      = sq_sz;

    /* Map CQ ring */
    size_t cq_sz = cq_ring_size(&be->params);
    void *cq_ptr = mmap_ring(fd, cq_sz, IORING_OFF_CQ_RING);
    if (!cq_ptr) goto fail;
    be->cq.head         = (uint32_t *)((char *)cq_ptr + be->params.cq_off.head);
    be->cq.tail         = (uint32_t *)((char *)cq_ptr + be->params.cq_off.tail);
    be->cq.ring_mask    = (uint32_t *)((char *)cq_ptr + be->params.cq_off.ring_mask);
    be->cq.ring_entries = (uint32_t *)((char *)cq_ptr + be->params.cq_off.ring_entries);
    be->cq.cqes         = (struct io_uring_cqe *)((char *)cq_ptr + be->params.cq_off.cqes);
    be->cq.ring_sz      = cq_sz;

    /* Map SQEs */
    size_t sqe_sz = be->params.sq_entries * sizeof(struct io_uring_sqe);
    void *sqe_ptr = mmap_ring(fd, sqe_sz, IORING_OFF_SQES);
    if (!sqe_ptr) goto fail;
    be->sq.sqes = sqe_ptr;
    be->sq.sqe_sz = sqe_sz;

    /* Initialise array indices */
    for (uint32_t i = 0; i < *be->sq.ring_entries; ++i) {
        be->sq.array[i] = i;
    }

    /* Fixed buffers */
    if (be->cfg.use_fixed_buf && be->cfg.fixed_buf_count > 0) {
        size_t total = be->cfg.fixed_buf_size * be->cfg.fixed_buf_count;
        /* Use mmap for huge-page friendly allocation */
        be->buf_pool.base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (be->buf_pool.base == MAP_FAILED) {
            be->buf_pool.base = NULL;
            goto fail;
        }
        be->buf_pool.buf_size  = be->cfg.fixed_buf_size;
        be->buf_pool.buf_count = be->cfg.fixed_buf_count;
        be->buf_pool.bid_map   = cwist_alloc_array(be->cfg.fixed_buf_count, sizeof(int));
        if (!be->buf_pool.bid_map) goto fail;
        memset(be->buf_pool.bid_map, 0, sizeof(int) * be->cfg.fixed_buf_count);
        be->buf_pool.free_count = be->cfg.fixed_buf_count;

        struct iovec iov = {
            .iov_base = be->buf_pool.base,
            .iov_len  = total
        };
        int rc = sys_io_uring_register(fd, IORING_REGISTER_BUFFERS, &iov, 1);
        if (rc < 0) {
            fprintf(stderr, "[io_uring] buffer register failed: %s (falling back to dynamic buffers)\n", strerror(-rc));
            /* Fall back: keep the pool but don't use fixed buffers in SQEs */
            be->cfg.use_fixed_buf = false;
        }
    }

    return be;

fail:
    cwist_uring_backend_destroy(be);
    return NULL;
}

void cwist_uring_backend_destroy(cwist_uring_backend_t *be) {
    if (!be) return;

    atomic_store_explicit(&be->stopped, true, memory_order_release);

    /* Unregister fixed buffers */
    if (be->buf_pool.base) {
        sys_io_uring_register(be->ring_fd, IORING_UNREGISTER_BUFFERS, NULL, 0);
        size_t total = be->buf_pool.buf_size * be->buf_pool.buf_count;
        munmap(be->buf_pool.base, total);
        cwist_free(be->buf_pool.bid_map);
    }

    /* Unmap rings */
    if (be->sq.sqes) munmap(be->sq.sqes, be->sq.sqe_sz);
    if (be->cq.cqes) munmap((char *)be->cq.cqes - be->params.cq_off.cqes, be->cq.ring_sz);
    if (be->sq.array) munmap((char *)be->sq.array - be->params.sq_off.array, be->sq.ring_sz);

    /* Free streams */
    cwist_core_stream_t *s = be->stream_list;
    while (s) {
        cwist_core_stream_t *next = s->next;
        cwist_free(s);
        s = next;
    }

    if (be->ring_fd >= 0) close(be->ring_fd);
    pthread_mutex_destroy(&be->stream_lock);
    cwist_free(be);
}

/* -------------------------------------------------------------------------
 * Stream management
 * ---------------------------------------------------------------------- */

cwist_core_stream_t *cwist_uring_stream_register(cwist_uring_backend_t *be,
                                                   int fd,
                                                   uint32_t proto) {
    if (!be || fd < 0) return NULL;
    cwist_core_stream_t *s = cwist_alloc(sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->fd = fd;
    s->protocol = proto;
    s->stream_id = (uint64_t)(uintptr_t)s; /**< simple unique id */

    pthread_mutex_lock(&be->stream_lock);
    s->next = be->stream_list;
    if (be->stream_list) be->stream_list->prev = s;
    be->stream_list = s;
    atomic_fetch_add_explicit(&be->stream_count, 1, memory_order_relaxed);
    pthread_mutex_unlock(&be->stream_lock);
    return s;
}

void cwist_uring_stream_unregister(cwist_uring_backend_t *be,
                                    cwist_core_stream_t *stream) {
    if (!be || !stream) return;
    pthread_mutex_lock(&be->stream_lock);
    if (stream->prev) stream->prev->next = stream->next;
    else be->stream_list = stream->next;
    if (stream->next) stream->next->prev = stream->prev;
    atomic_fetch_sub_explicit(&be->stream_count, 1, memory_order_relaxed);
    pthread_mutex_unlock(&be->stream_lock);
    cwist_free(stream);
}

/* -------------------------------------------------------------------------
 * SQE helpers
 * ---------------------------------------------------------------------- */

static struct io_uring_sqe *get_sqe(cwist_uring_backend_t *be) {
    struct cwist_uring_sq_ring *sq = &be->sq;
    uint32_t head = atomic_load_explicit((_Atomic uint32_t *)sq->head, memory_order_acquire);
    uint32_t next = *sq->tail + 1;
    if (next - head > *sq->ring_entries) {
        return NULL; /* SQ full */
    }
    struct io_uring_sqe *sqe = &sq->sqes[next & *sq->ring_mask];
    memset(sqe, 0, sizeof(*sqe));
    sqe->user_data = 0;
    *sq->tail = next;
    return sqe;
}

static bool flush_sq_internal(cwist_uring_backend_t *be) {
    struct cwist_uring_sq_ring *sq = &be->sq;
    if (*sq->tail == *sq->head) return true; /* empty */

    uint32_t tail = *sq->tail;
    atomic_store_explicit((_Atomic uint32_t *)sq->tail, tail, memory_order_release);

    int ret = sys_io_uring_enter(be->ring_fd, tail - *sq->head, 0,
                                  IORING_ENTER_GETEVENTS, NULL);
    if (ret < 0) {
        fprintf(stderr, "[io_uring] enter submit failed: %s\n", strerror(-ret));
        return false;
    }
    atomic_store_explicit(&be->pending_sqes, 0, memory_order_release);
    return true;
}

/* -------------------------------------------------------------------------
 * Submission API
 * ---------------------------------------------------------------------- */

bool cwist_uring_submit_recvmsg(cwist_uring_backend_t *be,
                                 cwist_core_stream_t *stream,
                                 struct msghdr *msg) {
    if (!be || !stream) return false;
    struct io_uring_sqe *sqe = get_sqe(be);
    if (!sqe) {
        if (!flush_sq_internal(be)) return false;
        sqe = get_sqe(be);
        if (!sqe) return false;
    }
    sqe->opcode = IORING_OP_RECVMSG;
    sqe->fd     = stream->fd;
    sqe->addr   = (unsigned long)msg;
    sqe->user_data = (uintptr_t)stream;

    if (be->cfg.use_fixed_buf) {
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = 0;
    }
    atomic_fetch_add_explicit(&be->pending_sqes, 1, memory_order_relaxed);
    return true;
}

bool cwist_uring_submit_sendmsg(cwist_uring_backend_t *be,
                                 cwist_core_stream_t *stream,
                                 struct msghdr *msg) {
    if (!be || !stream) return false;
    struct io_uring_sqe *sqe = get_sqe(be);
    if (!sqe) {
        if (!flush_sq_internal(be)) return false;
        sqe = get_sqe(be);
        if (!sqe) return false;
    }
    sqe->opcode = IORING_OP_SENDMSG;
    sqe->fd     = stream->fd;
    sqe->addr   = (unsigned long)msg;
    sqe->user_data = (uintptr_t)stream | 1ULL; /**< bit-0 marks send side */
    atomic_fetch_add_explicit(&be->pending_sqes, 1, memory_order_relaxed);
    return true;
}

bool cwist_uring_submit_read(cwist_uring_backend_t *be,
                              cwist_core_stream_t *stream,
                              void *buf,
                              size_t len,
                              off_t offset) {
    if (!be || !stream) return false;
    struct io_uring_sqe *sqe = get_sqe(be);
    if (!sqe) {
        if (!flush_sq_internal(be)) return false;
        sqe = get_sqe(be);
        if (!sqe) return false;
    }
    sqe->opcode = IORING_OP_READ;
    sqe->fd     = stream->fd;
    sqe->addr   = (unsigned long)buf;
    sqe->len    = (uint32_t)len;
    sqe->off    = (uint64_t)offset;
    sqe->user_data = (uintptr_t)stream;
    atomic_fetch_add_explicit(&be->pending_sqes, 1, memory_order_relaxed);
    return true;
}

bool cwist_uring_submit_write(cwist_uring_backend_t *be,
                               cwist_core_stream_t *stream,
                               const void *buf,
                               size_t len,
                               off_t offset) {
    if (!be || !stream) return false;
    struct io_uring_sqe *sqe = get_sqe(be);
    if (!sqe) {
        if (!flush_sq_internal(be)) return false;
        sqe = get_sqe(be);
        if (!sqe) return false;
    }
    sqe->opcode = IORING_OP_WRITE;
    sqe->fd     = stream->fd;
    sqe->addr   = (unsigned long)buf;
    sqe->len    = (uint32_t)len;
    sqe->off    = (uint64_t)offset;
    sqe->user_data = (uintptr_t)stream | 1ULL;
    atomic_fetch_add_explicit(&be->pending_sqes, 1, memory_order_relaxed);
    return true;
}

bool cwist_uring_submit_splice(cwist_uring_backend_t *be,
                                cwist_core_stream_t *stream_in,
                                cwist_core_stream_t *stream_out,
                                size_t len) {
    if (!be || !stream_in || !stream_out) return false;
    struct io_uring_sqe *sqe = get_sqe(be);
    if (!sqe) {
        if (!flush_sq_internal(be)) return false;
        sqe = get_sqe(be);
        if (!sqe) return false;
    }
    sqe->opcode = IORING_OP_SPLICE;
    sqe->fd     = stream_out->fd;       /* output fd */
    sqe->len    = (uint32_t)len;
    sqe->splice_fd_in = stream_in->fd;  /* input fd */
    sqe->off    = 0;                    /* output offset */
    sqe->splice_off_in = 0;             /* input offset */
    sqe->user_data = (uintptr_t)stream_out | 1ULL;
    atomic_fetch_add_explicit(&be->pending_sqes, 1, memory_order_relaxed);
    return true;
}

bool cwist_uring_submit_close(cwist_uring_backend_t *be, int fd) {
    if (!be || fd < 0) return false;
    struct io_uring_sqe *sqe = get_sqe(be);
    if (!sqe) {
        if (!flush_sq_internal(be)) return false;
        sqe = get_sqe(be);
        if (!sqe) return false;
    }
    sqe->opcode = IORING_OP_CLOSE;
    sqe->fd     = fd;
    sqe->user_data = 0;
    atomic_fetch_add_explicit(&be->pending_sqes, 1, memory_order_relaxed);
    return true;
}

int cwist_uring_flush_sq(cwist_uring_backend_t *be) {
    if (!be) return -EINVAL;
    uint32_t pending = atomic_load_explicit(&be->pending_sqes, memory_order_acquire);
    if (pending == 0) return 0;
    if (!flush_sq_internal(be)) return -EIO;
    return (int)pending;
}

/* -------------------------------------------------------------------------
 * Completion processing
 * ---------------------------------------------------------------------- */

static void process_cqe(cwist_uring_backend_t *be, struct io_uring_cqe *cqe) {
    (void)be;
    uint64_t ud = cqe->user_data;
    if (!ud) return; /* anonymous op (e.g., close) */

    bool is_send = (ud & 1ULL);
    cwist_core_stream_t *stream = (cwist_core_stream_t *)(uintptr_t)(ud & ~1ULL);
    if (!stream) return;

    cwist_uring_cqe_cb cb = is_send ? stream->send_cb : stream->recv_cb;
    if (cb) cb(stream, cqe->res, cqe->flags, stream->user_data);

    /* If fixed buffer was used, release it */
    if (cqe->flags & IORING_CQE_F_BUFFER) {
        uint16_t bid = cqe->flags >> 16;
        cwist_uring_buf_release(be, bid);
    }
}

int cwist_uring_poll_cq(cwist_uring_backend_t *be,
                         unsigned min_wait,
                         int timeout_ms) {
    (void)timeout_ms;
    if (!be) return -EINVAL;

    /* Ensure submissions are visible to the kernel before waiting */
    if (atomic_load_explicit(&be->pending_sqes, memory_order_acquire) > 0) {
        flush_sq_internal(be);
    }

    unsigned flags = 0;
    if (min_wait > 0) flags |= IORING_ENTER_GETEVENTS;

    int ret = sys_io_uring_enter(be->ring_fd, 0, min_wait, flags, NULL);
    if (ret < 0) {
        if (-ret == EAGAIN || -ret == EINTR) return 0;
        return ret;
    }

    struct cwist_uring_cq_ring *cq = &be->cq;
    uint32_t head = atomic_load_explicit((_Atomic uint32_t *)cq->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit((_Atomic uint32_t *)cq->tail, memory_order_acquire);
    int processed = 0;

    while (head != tail) {
        struct io_uring_cqe *cqe = &cq->cqes[head & *cq->ring_mask];
        process_cqe(be, cqe);
        head++;
        processed++;
    }
    atomic_store_explicit((_Atomic uint32_t *)cq->head, head, memory_order_release);
    return processed;
}

void cwist_uring_backend_run(cwist_uring_backend_t *be) {
    if (!be) return;
    atomic_store_explicit(&be->running, true, memory_order_release);

    while (!atomic_load_explicit(&be->stopped, memory_order_acquire)) {
        /* Flush any queued SQEs */
        if (atomic_load_explicit(&be->pending_sqes, memory_order_acquire) > 0) {
            flush_sq_internal(be);
        }

        /* Poll with a short timeout so we can check stopped periodically */
        int n = cwist_uring_poll_cq(be, 1, 1);
        if (n < 0 && n != -EAGAIN && n != -EINTR) {
            fprintf(stderr, "[io_uring] poll error: %d\n", n);
            break;
        }
    }

    atomic_store_explicit(&be->running, false, memory_order_release);
}

void cwist_uring_backend_stop(cwist_uring_backend_t *be) {
    if (be) atomic_store_explicit(&be->stopped, true, memory_order_release);
}

/* -------------------------------------------------------------------------
 * Fixed buffer pool
 * ---------------------------------------------------------------------- */

bool cwist_uring_buf_acquire(cwist_uring_backend_t *be, uint16_t *bid, void **ptr) {
    if (!be || !bid || !ptr) return false;
    cwist_uring_buf_pool_t *pool = &be->buf_pool;
    if (!pool->base || pool->free_count == 0) return false;

    for (size_t i = 0; i < pool->buf_count; ++i) {
        if (pool->bid_map[i] == 0) {
            pool->bid_map[i] = 1;
            pool->free_count--;
            *bid = (uint16_t)i;
            *ptr = pool->base + i * pool->buf_size;
            return true;
        }
    }
    return false;
}

void cwist_uring_buf_release(cwist_uring_backend_t *be, uint16_t bid) {
    if (!be) return;
    cwist_uring_buf_pool_t *pool = &be->buf_pool;
    if (!pool->base || bid >= pool->buf_count) return;
    if (pool->bid_map[bid] != 0) {
        pool->bid_map[bid] = 0;
        pool->free_count++;
    }
}

uint32_t cwist_uring_pending_sqes(const cwist_uring_backend_t *be) {
    if (!be) return 0;
    return atomic_load_explicit((_Atomic uint32_t *)&be->pending_sqes, memory_order_acquire);
}

uint32_t cwist_uring_active_buffers(const cwist_uring_backend_t *be) {
    if (!be || !be->buf_pool.base) return 0;
    return (uint32_t)(be->buf_pool.buf_count - be->buf_pool.free_count);
}
