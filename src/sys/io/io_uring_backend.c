#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cwist/sys/io/io_uring_backend.h>
#include <cwist/core/mem/alloc.h>
#include <ttak/mem/mem.h>
#include <ttak/timing/timing.h>
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
#include <time.h>

/**
 * @file io_uring_backend.c
 * @brief io_uring backend with generation-tagged two-phase demolition protocol.
 */

static inline int sys_io_uring_setup(unsigned entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static inline int sys_io_uring_enter(int ring_fd, unsigned to_submit,
                                      unsigned min_complete, unsigned flags, sigset_t *sig) {
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, sig);
}

static inline int sys_io_uring_register(int ring_fd, unsigned opcode, const void *arg, unsigned nr_args) {
    return (int)syscall(__NR_io_uring_register, ring_fd, opcode, arg, nr_args);
}

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

struct cwist_uring_backend {
    int ring_fd;
    struct cwist_uring_sq_ring sq;
    struct cwist_uring_cq_ring cq;
    struct io_uring_params params;
    cwist_uring_config_t cfg;

    cwist_core_stream_t *streams;
    uint32_t            *free_stack;
    atomic_uint          free_top;
    atomic_uint          global_gen;
    pthread_mutex_t      stream_lock;

    cwist_uring_buf_pool_t buf_pool;
    uint32_t             sq_tail;
    atomic_uint          pending_sqes;
    atomic_bool          stopped;
    atomic_bool          running;
};

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

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

cwist_uring_backend_t *cwist_uring_backend_create(const cwist_uring_config_t *cfg_in) {
    uint64_t now = ttak_get_tick_count();
    cwist_uring_backend_t *be = ttak_mem_alloc(sizeof(*be), __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!be) return NULL;

    be->cfg = cfg_in ? *cfg_in : (cwist_uring_config_t)CWIST_URING_CONFIG_DEFAULT;
    atomic_init(&be->pending_sqes, 0);
    atomic_init(&be->stopped, false);
    atomic_init(&be->running, false);
    atomic_init(&be->global_gen, 1);
    pthread_mutex_init(&be->stream_lock, NULL);

    size_t streams_sz = be->cfg.max_streams * sizeof(cwist_core_stream_t);
    be->streams = ttak_mem_alloc(streams_sz, __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!be->streams) goto fail;

    size_t stack_sz = be->cfg.max_streams * sizeof(uint32_t);
    be->free_stack = ttak_mem_alloc(stack_sz, __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!be->free_stack) goto fail;

    for (uint32_t i = 0; i < be->cfg.max_streams; ++i) {
        be->free_stack[i] = (be->cfg.max_streams - 1) - i;
        be->streams[i].index = i;
    }
    atomic_init(&be->free_top, be->cfg.max_streams);

    memset(&be->params, 0, sizeof(be->params));
    if (be->cfg.sqpoll) {
        be->params.flags |= IORING_SETUP_SQPOLL;
        be->params.sq_thread_idle = 2000;
    }
    if (be->cfg.iopoll) be->params.flags |= IORING_SETUP_IOPOLL;

    int fd = sys_io_uring_setup(be->cfg.sq_entries, &be->params);
    if (fd < 0) goto fail;
    be->ring_fd = fd;

    size_t sq_sz = sq_ring_size(&be->params);
    void *sq_ptr = mmap_ring(fd, sq_sz, IORING_OFF_SQ_RING);
    if (!sq_ptr) goto fail;
    be->sq.head         = (uint32_t *)((char *)sq_ptr + be->params.sq_off.head);
    be->sq.tail         = (uint32_t *)((char *)sq_ptr + be->params.sq_off.tail);
    be->sq.ring_mask    = (uint32_t *)((char *)sq_ptr + be->params.sq_off.ring_mask);
    be->sq.ring_entries = (uint32_t *)((char *)sq_ptr + be->params.sq_off.ring_entries);
    be->sq.array        = (uint32_t *)((char *)sq_ptr + be->params.sq_off.array);
    be->sq.ring_sz      = sq_sz;

    size_t cq_sz = cq_ring_size(&be->params);
    void *cq_ptr = mmap_ring(fd, cq_sz, IORING_OFF_CQ_RING);
    if (!cq_ptr) goto fail;
    be->cq.head         = (uint32_t *)((char *)cq_ptr + be->params.cq_off.head);
    be->cq.tail         = (uint32_t *)((char *)cq_ptr + be->params.cq_off.tail);
    be->cq.ring_mask    = (uint32_t *)((char *)cq_ptr + be->params.cq_off.ring_mask);
    be->cq.ring_entries = (uint32_t *)((char *)cq_ptr + be->params.cq_off.ring_entries);
    be->cq.cqes         = (struct io_uring_cqe *)((char *)cq_ptr + be->params.cq_off.cqes);
    be->cq.ring_sz      = cq_sz;

    size_t sqe_sz = be->params.sq_entries * sizeof(struct io_uring_sqe);
    void *sqe_ptr = mmap_ring(fd, sqe_sz, IORING_OFF_SQES);
    if (!sqe_ptr) goto fail;
    be->sq.sqes = sqe_ptr;
    be->sq.sqe_sz = sqe_sz;

    be->sq_tail = *be->sq.tail;
    for (uint32_t i = 0; i < *be->sq.ring_entries; ++i) be->sq.array[i] = i;

    if (be->cfg.use_fixed_buf && be->cfg.fixed_buf_count > 0) {
        size_t total = be->cfg.fixed_buf_size * be->cfg.fixed_buf_count;
        be->buf_pool.base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (be->buf_pool.base != MAP_FAILED) {
            be->buf_pool.buf_size  = be->cfg.fixed_buf_size;
            be->buf_pool.buf_count = be->cfg.fixed_buf_count;
            be->buf_pool.free_stack = ttak_mem_alloc(be->cfg.fixed_buf_count * sizeof(uint32_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
            if (be->buf_pool.free_stack) {
                for (uint32_t i = 0; i < be->cfg.fixed_buf_count; ++i)
                    be->buf_pool.free_stack[i] = (be->cfg.fixed_buf_count - 1) - i;
                atomic_init(&be->buf_pool.free_top, be->cfg.fixed_buf_count);
                be->buf_pool.free_count = be->cfg.fixed_buf_count;
                struct iovec iov = {.iov_base = be->buf_pool.base, .iov_len = total};
                if (sys_io_uring_register(fd, IORING_REGISTER_BUFFERS, &iov, 1) < 0) be->cfg.use_fixed_buf = false;
            }
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
    if (be->buf_pool.base) {
        sys_io_uring_register(be->ring_fd, IORING_UNREGISTER_BUFFERS, NULL, 0);
        munmap(be->buf_pool.base, be->buf_pool.buf_size * be->buf_pool.buf_count);
        ttak_mem_free(be->buf_pool.free_stack);
    }
    if (be->sq.sqes) munmap(be->sq.sqes, be->sq.sqe_sz);
    if (be->cq.cqes) munmap((char *)be->cq.cqes - be->params.cq_off.cqes, be->cq.ring_sz);
    if (be->sq.array) munmap((char *)be->sq.array - be->params.sq_off.array, be->sq.ring_sz);
    if (be->streams) ttak_mem_free(be->streams);
    if (be->free_stack) ttak_mem_free(be->free_stack);
    if (be->ring_fd >= 0) close(be->ring_fd);
    pthread_mutex_destroy(&be->stream_lock);
    ttak_mem_free(be);
}

static void finalize_stream_destruction(cwist_uring_backend_t *be, cwist_core_stream_t *stream) {
    if (stream->fd >= 0) {
        close(stream->fd);
        stream->fd = -1;
    }
    pthread_mutex_lock(&be->stream_lock);
    uint32_t top = atomic_load_explicit(&be->free_top, memory_order_acquire);
    be->free_stack[top++] = stream->index;
    atomic_store_explicit(&be->free_top, top, memory_order_release);
    pthread_mutex_unlock(&be->stream_lock);
}

cwist_core_stream_t *cwist_uring_stream_register(cwist_uring_backend_t *be, int fd, uint32_t proto) {
    if (!be || fd < 0) return NULL;
    pthread_mutex_lock(&be->stream_lock);
    uint32_t top = atomic_load_explicit(&be->free_top, memory_order_acquire);
    if (top == 0) {
        pthread_mutex_unlock(&be->stream_lock);
        return NULL;
    }
    uint32_t idx = be->free_stack[--top];
    atomic_store_explicit(&be->free_top, top, memory_order_release);
    pthread_mutex_unlock(&be->stream_lock);

    cwist_core_stream_t *s = &be->streams[idx];
    uint32_t index = s->index;
    memset(s, 0, sizeof(*s));
    s->index = index;
    s->fd = fd;
    s->protocol = proto;
    s->generation = atomic_fetch_add_explicit(&be->global_gen, 1, memory_order_relaxed);
    atomic_init(&s->pending_io_count, 0);
    s->deadline_ts = get_time_ms() + 3000;
    return s;
}

void cwist_uring_stream_unregister(cwist_uring_backend_t *be, cwist_core_stream_t *stream) {
    if (!be || !stream) return;
    stream->is_dead = true;
    if (atomic_load_explicit(&stream->pending_io_count, memory_order_acquire) == 0) finalize_stream_destruction(be, stream);
}

static struct io_uring_sqe *get_sqe(cwist_uring_backend_t *be) {
    struct cwist_uring_sq_ring *sq = &be->sq;
    uint32_t head = atomic_load_explicit((_Atomic uint32_t *)sq->head, memory_order_acquire);
    if (be->sq_tail - head >= *sq->ring_entries) return NULL;
    struct io_uring_sqe *sqe = &sq->sqes[be->sq_tail & *sq->ring_mask];
    memset(sqe, 0, sizeof(*sqe));
    be->sq_tail++;
    return sqe;
}

static bool flush_sq_internal(cwist_uring_backend_t *be) {
    struct cwist_uring_sq_ring *sq = &be->sq;
    uint32_t kernel_tail = atomic_load_explicit((_Atomic uint32_t *)sq->tail, memory_order_relaxed);
    if (be->sq_tail == kernel_tail) return true;
    atomic_store_explicit((_Atomic uint32_t *)sq->tail, be->sq_tail, memory_order_release);
    if (sys_io_uring_enter(be->ring_fd, be->sq_tail - atomic_load_explicit((_Atomic uint32_t *)sq->head, memory_order_acquire), 0, IORING_ENTER_GETEVENTS, NULL) < 0) return false;
    atomic_store_explicit(&be->pending_sqes, 0, memory_order_release);
    return true;
}

#define TICKET_SEND_BIT    (1ULL)
#define ENCODE_TICKET(g, i, s) (((uint64_t)(g) << 32) | ((uint64_t)(i) << 1) | ((s) ? TICKET_SEND_BIT : 0))
#define DECODE_GEN(t)      ((uint32_t)((t) >> 32))
#define DECODE_IDX(t)      ((uint32_t)(((t) & 0xFFFFFFFFULL) >> 1))
#define DECODE_IS_SEND(t)  ((bool)((t) & TICKET_SEND_BIT))

static bool submit_op(cwist_uring_backend_t *be, cwist_core_stream_t *stream, uint8_t opcode, void *addr, uint32_t len, uint64_t off, bool is_send) {
    if (!be || !stream || stream->is_dead) return false;
    struct io_uring_sqe *sqe = get_sqe(be);
    if (!sqe) {
        if (!flush_sq_internal(be)) return false;
        sqe = get_sqe(be);
        if (!sqe) return false;
    }
    atomic_fetch_add_explicit(&stream->pending_io_count, 1, memory_order_relaxed);
    sqe->opcode = opcode;
    sqe->fd = stream->fd;
    sqe->addr = (unsigned long)addr;
    sqe->len = len;
    sqe->off = off;
    sqe->user_data = ENCODE_TICKET(stream->generation, stream->index, is_send);
    atomic_fetch_add_explicit(&be->pending_sqes, 1, memory_order_relaxed);
    return true;
}

bool cwist_uring_submit_recvmsg(cwist_uring_backend_t *be, cwist_core_stream_t *stream, struct msghdr *msg) {
    return submit_op(be, stream, IORING_OP_RECVMSG, msg, 1, 0, false);
}

bool cwist_uring_submit_sendmsg(cwist_uring_backend_t *be, cwist_core_stream_t *stream, struct msghdr *msg) {
    return submit_op(be, stream, IORING_OP_SENDMSG, msg, 1, 0, true);
}

bool cwist_uring_submit_read(cwist_uring_backend_t *be, cwist_core_stream_t *stream, void *buf, size_t len, off_t offset) {
    return submit_op(be, stream, IORING_OP_READ, buf, (uint32_t)len, (uint64_t)offset, false);
}

bool cwist_uring_submit_write(cwist_uring_backend_t *be, cwist_core_stream_t *stream, const void *buf, size_t len, off_t offset) {
    return submit_op(be, stream, IORING_OP_WRITE, (void *)buf, (uint32_t)len, (uint64_t)offset, true);
}

bool cwist_uring_submit_nop(cwist_uring_backend_t *be, cwist_core_stream_t *stream) {
    return submit_op(be, stream, IORING_OP_NOP, NULL, 0, 0, false);
}

bool cwist_uring_submit_splice(cwist_uring_backend_t *be, cwist_core_stream_t *stream_in, cwist_core_stream_t *stream_out, size_t len) {
    if (!be || !stream_in || !stream_out || stream_out->is_dead) return false;
    struct io_uring_sqe *sqe = get_sqe(be);
    if (!sqe) {
        if (!flush_sq_internal(be)) return false;
        sqe = get_sqe(be);
        if (!sqe) return false;
    }
    atomic_fetch_add_explicit(&stream_out->pending_io_count, 1, memory_order_relaxed);
    sqe->opcode = IORING_OP_SPLICE;
    sqe->fd = stream_out->fd;
    sqe->len = (uint32_t)len;
    sqe->splice_fd_in = stream_in->fd;
    sqe->user_data = ENCODE_TICKET(stream_out->generation, stream_out->index, true);
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
    sqe->fd = fd;
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

static void process_cqe(cwist_uring_backend_t *be, struct io_uring_cqe *cqe) {
    uint64_t ud = cqe->user_data;
    if (!ud) return;
    uint32_t gen = DECODE_GEN(ud), idx = DECODE_IDX(ud);
    bool is_send = DECODE_IS_SEND(ud);
    if (idx >= be->cfg.max_streams) return;
    cwist_core_stream_t *stream = &be->streams[idx];
    if (stream->generation != gen) return;
    stream->deadline_ts = get_time_ms() + 3000;
    if (cqe->res < 0 && cqe->res != -EAGAIN && cqe->res != -EINTR && cqe->res != -ECANCELED) stream->is_dead = true;
    cwist_uring_cqe_cb cb = is_send ? stream->send_cb : stream->recv_cb;
    if (cb) cb(stream, cqe->res, cqe->flags, stream->user_data);
    if (cqe->flags & IORING_CQE_F_BUFFER) cwist_uring_buf_release(be, (uint16_t)(cqe->flags >> 16));
    if (atomic_fetch_sub_explicit(&stream->pending_io_count, 1, memory_order_acq_rel) == 1) {
        if (stream->is_dead) finalize_stream_destruction(be, stream);
    }
}

int cwist_uring_poll_cq(cwist_uring_backend_t *be, unsigned min_wait, int timeout_ms) {
    (void)timeout_ms;
    if (!be) return -EINVAL;
    if (atomic_load_explicit(&be->pending_sqes, memory_order_acquire) > 0) flush_sq_internal(be);
    unsigned flags = (min_wait > 0) ? IORING_ENTER_GETEVENTS : 0;
    if (sys_io_uring_enter(be->ring_fd, 0, min_wait, flags, NULL) < 0) return -errno;
    struct cwist_uring_cq_ring *cq = &be->cq;
    uint32_t head = atomic_load_explicit((_Atomic uint32_t *)cq->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit((_Atomic uint32_t *)cq->tail, memory_order_acquire);
    int processed = 0;
    while (head != tail) {
        process_cqe(be, &cq->cqes[head & *cq->ring_mask]);
        head++;
        processed++;
    }
    atomic_store_explicit((_Atomic uint32_t *)cq->head, head, memory_order_release);
    return processed;
}

void cwist_uring_backend_run(cwist_uring_backend_t *be) {
    if (!be) return;
    atomic_store_explicit(&be->running, true, memory_order_release);
    uint64_t last_sweep = get_time_ms();
    while (!atomic_load_explicit(&be->stopped, memory_order_acquire)) {
        if (atomic_load_explicit(&be->pending_sqes, memory_order_acquire) > 0) flush_sq_internal(be);
        if (cwist_uring_poll_cq(be, 1, 100) < 0) break;
        uint64_t now = get_time_ms();
        if (now - last_sweep >= 1000) {
            for (uint32_t i = 0; i < be->cfg.max_streams; ++i) {
                cwist_core_stream_t *s = &be->streams[i];
                if (s->fd >= 0 && !s->is_dead && now > s->deadline_ts) cwist_uring_stream_unregister(be, s);
            }
            last_sweep = now;
        }
    }
    atomic_store_explicit(&be->running, false, memory_order_release);
}

void cwist_uring_backend_stop(cwist_uring_backend_t *be) {
    if (be) atomic_store_explicit(&be->stopped, true, memory_order_release);
}

bool cwist_uring_buf_acquire(cwist_uring_backend_t *be, uint16_t *bid, void **ptr) {
    if (!be || !bid || !ptr || !be->buf_pool.base || be->buf_pool.free_count == 0) return false;
    uint32_t top = atomic_load_explicit(&be->buf_pool.free_top, memory_order_acquire);
    if (top == 0) return false;
    top--;
    uint32_t idx = be->buf_pool.free_stack[top];
    atomic_store_explicit(&be->buf_pool.free_top, top, memory_order_release);
    be->buf_pool.free_count--;
    *bid = (uint16_t)idx;
    *ptr = be->buf_pool.base + idx * be->buf_pool.buf_size;
    return true;
}

void cwist_uring_buf_release(cwist_uring_backend_t *be, uint16_t bid) {
    if (!be || !be->buf_pool.base || bid >= be->buf_pool.buf_count) return;
    uint32_t top = atomic_load_explicit(&be->buf_pool.free_top, memory_order_acquire);
    be->buf_pool.free_stack[top] = bid;
    atomic_store_explicit(&be->buf_pool.free_top, top + 1, memory_order_release);
    be->buf_pool.free_count++;
}

uint32_t cwist_uring_pending_sqes(const cwist_uring_backend_t *be) {
    return be ? atomic_load_explicit((_Atomic uint32_t *)&be->pending_sqes, memory_order_acquire) : 0;
}

uint32_t cwist_uring_active_buffers(const cwist_uring_backend_t *be) {
    return (be && be->buf_pool.base) ? (uint32_t)(be->buf_pool.buf_count - be->buf_pool.free_count) : 0;
}
