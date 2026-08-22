/**
 * @file reactor.c
 * @brief Readiness multiplexer (io_uring / epoll / kqueue) driving CWIST's
 * synchronous callback model.
 *
 * Design note — why readiness notification + synchronous completion is kept:
 * 1. No queues: a request passes through no queue at all; the woken worker
 *    finishes it immediately. This is the source of the 0.0x ms latency.
 *    A completion model pushes each request through a ring 3-4 times and
 *    binds progress to loop ticks, landing at 2-3ms (Axum/Tokio level).
 * 2. Structural backpressure: callbacks block, so unfinished work cannot
 *    pile up in kernel or userland. A single in-flight cap per thread
 *    (32 in http.c) is all the flow control the server needs.
 * 3. Cache locality: the whole request lifetime runs on one thread's
 *    contiguous stack, reusing L1/L2. A completion model splits the
 *    handler into fragments and lifts per-stage state onto the heap.
 * 4. No state machines: handlers are straight-line code; a stack trace
 *    is the request's execution history.
 * 5. Deterministic tail: with no queue waiting, p99/p999 converge on the
 *    mean.
 * The cost: per-connection concurrency is bounded by worker count
 * (cores*8), covered by multi-process scaling (fork + SO_REUSEPORT).
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/io/reactor.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/sys/app/shutdown.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>

#ifdef __linux__
#include <sys/syscall.h>
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/epoll.h>

static inline int sys_io_uring_setup(unsigned entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}
static inline int sys_io_uring_enter(int ring_fd, unsigned to_submit, unsigned min_complete, unsigned flags, sigset_t *sig) {
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, sig);
}

/* Ring setup helpers absorbed from the retired io_uring_backend.c. */
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

typedef struct {
    int ring_fd;
    /* Serializes SQ producers: the accept thread (reactor_add) and worker
     * threads (rearm/del) submit concurrently; without this the SQE memcpy
     * and tail advance race and one submission overwrites the other,
     * silently dropping the connection's POLL_ADD. */
    pthread_mutex_t sq_lock;
    struct io_uring_sqe *sqes;
    struct io_uring_cqe *cqes;
    uint32_t *sq_head, *sq_tail, *sq_ring_mask, *sq_array;
    uint32_t *cq_head, *cq_tail, *cq_ring_mask;
    uint32_t sq_entries;
    uint32_t cq_entries;
    size_t sq_ring_sz, cq_ring_sz, sqes_sz;
    bool active;

    // epoll fallback
    int epoll_fd;
    bool use_epoll;
} reactor_impl_t;

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/event.h>

typedef struct {
    int kq_fd;
} reactor_impl_t;

#else
// Fallback
typedef struct {
} reactor_impl_t;
#endif

#define MAX_REACTOR_EVENTS 4096

typedef struct {
    int fd;
    cwist_reactor_cb_t cb;
    void *ctx;
} reactor_event_ctx_t;

struct cwist_reactor {
    reactor_impl_t impl;
    bool running;
    reactor_event_ctx_t event_pool[MAX_REACTOR_EVENTS];
    /* Free-stack over event_pool (pattern absorbed from io_uring_backend.c's
     * stream table): O(1) slot acquire/release instead of a linear scan. */
    uint32_t free_stack[MAX_REACTOR_EVENTS];
    uint32_t free_top;
    pthread_mutex_t pool_lock;
};

static reactor_event_ctx_t *alloc_reactor_ctx(cwist_reactor_t *r, int fd, cwist_reactor_cb_t cb, void *ctx) {
    if (!r) return NULL;
    pthread_mutex_lock(&r->pool_lock);
    if (r->free_top == 0) {
        pthread_mutex_unlock(&r->pool_lock);
        return NULL;
    }
    uint32_t idx = r->free_stack[--r->free_top];
    pthread_mutex_unlock(&r->pool_lock);

    reactor_event_ctx_t *ev_ctx = &r->event_pool[idx];
    ev_ctx->fd = fd;
    ev_ctx->cb = cb;
    ev_ctx->ctx = ctx;
    return ev_ctx;
}

static void free_reactor_ctx(cwist_reactor_t *r, reactor_event_ctx_t *ev_ctx) {
    if (!r || !ev_ctx) return;
    uint32_t idx = (uint32_t)(ev_ctx - r->event_pool);
    pthread_mutex_lock(&r->pool_lock);
    r->free_stack[r->free_top++] = idx;
    pthread_mutex_unlock(&r->pool_lock);
}

cwist_reactor_t *cwist_reactor_create(void) {
    cwist_reactor_t *r = cwist_alloc(sizeof(cwist_reactor_t));
    if (!r) return NULL;
    memset(r, 0, sizeof(cwist_reactor_t));
    r->running = false;
    pthread_mutex_init(&r->pool_lock, NULL);
#ifdef __linux__
    pthread_mutex_init(&r->impl.sq_lock, NULL);
#endif
    for (uint32_t i = 0; i < MAX_REACTOR_EVENTS; i++) {
        r->free_stack[i] = (MAX_REACTOR_EVENTS - 1) - i;
    }
    r->free_top = MAX_REACTOR_EVENTS;

#ifdef __linux__
    r->impl.use_epoll = false;
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    int fd = sys_io_uring_setup(4096, &p);
    if (fd >= 0) {
        r->impl.ring_fd = fd;
        r->impl.sq_ring_sz = sq_ring_size(&p);
        r->impl.cq_ring_sz = cq_ring_size(&p);
        r->impl.sqes_sz = p.sq_entries * sizeof(struct io_uring_sqe);

        void *sq_ptr = mmap_ring(fd, r->impl.sq_ring_sz, IORING_OFF_SQ_RING);
        void *cq_ptr = mmap_ring(fd, r->impl.cq_ring_sz, IORING_OFF_CQ_RING);
        void *sqes_ptr = mmap_ring(fd, r->impl.sqes_sz, IORING_OFF_SQES);

        if (sq_ptr && cq_ptr && sqes_ptr) {
            r->impl.sq_head         = (uint32_t *)((char *)sq_ptr + p.sq_off.head);
            r->impl.sq_tail         = (uint32_t *)((char *)sq_ptr + p.sq_off.tail);
            r->impl.sq_ring_mask    = (uint32_t *)((char *)sq_ptr + p.sq_off.ring_mask);
            r->impl.sq_array        = (uint32_t *)((char *)sq_ptr + p.sq_off.array);
            r->impl.sqes            = sqes_ptr;
            r->impl.sq_entries      = p.sq_entries;

            r->impl.cq_head         = (uint32_t *)((char *)cq_ptr + p.cq_off.head);
            r->impl.cq_tail         = (uint32_t *)((char *)cq_ptr + p.cq_off.tail);
            r->impl.cq_ring_mask    = (uint32_t *)((char *)cq_ptr + p.cq_off.ring_mask);
            r->impl.cqes            = (struct io_uring_cqe *)((char *)cq_ptr + p.cq_off.cqes);
            r->impl.cq_entries      = p.cq_entries;
            r->impl.active = true;

            /* Identity SQE mapping is fixed for the ring's lifetime; fill it
             * once here instead of rewriting sq_array on every submission. */
            for (uint32_t i = 0; i < p.sq_entries; i++) r->impl.sq_array[i] = i;
        } else {
            if (sq_ptr) munmap(sq_ptr, r->impl.sq_ring_sz);
            if (cq_ptr) munmap(cq_ptr, r->impl.cq_ring_sz);
            if (sqes_ptr) munmap(sqes_ptr, r->impl.sqes_sz);
            close(fd);
            r->impl.use_epoll = true;
        }
    } else {
        r->impl.use_epoll = true;
    }

    if (r->impl.use_epoll) {
        r->impl.epoll_fd = epoll_create1(0);
        if (r->impl.epoll_fd < 0) {
            pthread_mutex_destroy(&r->pool_lock);
            cwist_free(r);
            return NULL;
        }
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    r->impl.kq_fd = kqueue();
    if (r->impl.kq_fd < 0) {
        pthread_mutex_destroy(&r->pool_lock);
        cwist_free(r);
        return NULL;
    }
#endif
    return r;
}

void cwist_reactor_destroy(cwist_reactor_t *reactor) {
    if (!reactor) return;
#ifdef __linux__
    if (!reactor->impl.use_epoll) {
        /* Teardown absorbed from io_uring_backend.c: unmap all three rings. */
        if (reactor->impl.sqes) munmap(reactor->impl.sqes, reactor->impl.sqes_sz);
        if (reactor->impl.cqes) {
            munmap((char *)reactor->impl.cqes - (reactor->impl.cq_ring_sz -
                   reactor->impl.cq_entries * sizeof(struct io_uring_cqe)),
                   reactor->impl.cq_ring_sz);
        }
        if (reactor->impl.sq_array) {
            munmap((char *)reactor->impl.sq_array - (reactor->impl.sq_ring_sz -
                   reactor->impl.sq_entries * sizeof(uint32_t)),
                   reactor->impl.sq_ring_sz);
        }
        close(reactor->impl.ring_fd);
    } else {
        close(reactor->impl.epoll_fd);
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    close(reactor->impl.kq_fd);
#endif
    pthread_mutex_destroy(&reactor->pool_lock);
#ifdef __linux__
    pthread_mutex_destroy(&reactor->impl.sq_lock);
#endif
    cwist_free(reactor);
}

#ifdef __linux__
/* Submit one SQE immediately: the reactor never batches or defers submission,
 * so a woken worker always sees the event on its next wait. */
static bool uring_submit(cwist_reactor_t *reactor, struct io_uring_sqe *out_sqe) {
    bool ok = false;
    pthread_mutex_lock(&reactor->impl.sq_lock);
    uint32_t tail = *reactor->impl.sq_tail;
    uint32_t head = __atomic_load_n(reactor->impl.sq_head, __ATOMIC_ACQUIRE);
    if (tail - head < reactor->impl.sq_entries) {
        uint32_t index = tail & *reactor->impl.sq_ring_mask;
        struct io_uring_sqe *sqe = &reactor->impl.sqes[index];
        memcpy(sqe, out_sqe, sizeof(*sqe));
        __atomic_store_n(reactor->impl.sq_tail, tail + 1, __ATOMIC_RELEASE);
        if (sys_io_uring_enter(reactor->impl.ring_fd, 1, 0, 0, NULL) < 0) {
            __atomic_store_n(reactor->impl.sq_tail, tail, __ATOMIC_RELEASE);
        } else {
            ok = true;
        }
    }
    pthread_mutex_unlock(&reactor->impl.sq_lock);
    return ok;
}
#endif

bool cwist_reactor_add(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb, void *ctx) {
    if (!reactor || fd < 0) return false;
    reactor_event_ctx_t *ev_ctx = alloc_reactor_ctx(reactor, fd, cb, ctx);
    if (!ev_ctx) return false;

#ifdef __linux__
    if (!reactor->impl.use_epoll) {
        /* One-shot POLL_ADD: multishot (IORING_POLL_ADD_MULTI) was rejected
         * because the callback owns ctx and frees it after firing, so a
         * persistent poll would re-dispatch into a recycled slot. */
        struct io_uring_sqe sqe;
        memset(&sqe, 0, sizeof(sqe));
        sqe.opcode = IORING_OP_POLL_ADD;
        sqe.fd = fd;
        sqe.poll_events = POLLIN;
        sqe.user_data = (uint64_t)ev_ctx;
        if (uring_submit(reactor, &sqe)) {
            return true;
        }
        free_reactor_ctx(reactor, ev_ctx);
        return false;
    } else {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        ev.data.ptr = ev_ctx;
        if (epoll_ctl(reactor->impl.epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0) {
            return true;
        }
        if (errno == EEXIST) {
            if (epoll_ctl(reactor->impl.epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0) {
                return true;
            }
        }
        free_reactor_ctx(reactor, ev_ctx);
        return false;
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    struct kevent change;
    EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_CLEAR | EV_ONESHOT, 0, 0, ev_ctx);
    if (kevent(reactor->impl.kq_fd, &change, 1, NULL, 0, NULL) == 0) {
        return true;
    }
    free_reactor_ctx(reactor, ev_ctx);
    return false;
#endif
    free_reactor_ctx(reactor, ev_ctx);
    return false;
}

bool cwist_reactor_mod(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb, void *ctx) {
    if (!reactor || fd < 0) return false;
    reactor_event_ctx_t *ev_ctx = alloc_reactor_ctx(reactor, fd, cb, ctx);
    if (!ev_ctx) return false;

#ifdef __linux__
    if (!reactor->impl.use_epoll) {
        struct io_uring_sqe sqe;
        memset(&sqe, 0, sizeof(sqe));
        sqe.opcode = IORING_OP_POLL_ADD;
        sqe.fd = fd;
        sqe.poll_events = POLLIN;
        sqe.user_data = (uint64_t)ev_ctx;
        if (uring_submit(reactor, &sqe)) {
            return true;
        }
        free_reactor_ctx(reactor, ev_ctx);
        return false;
    } else {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        ev.data.ptr = ev_ctx;
        if (epoll_ctl(reactor->impl.epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0) {
            return true;
        }
        free_reactor_ctx(reactor, ev_ctx);
        return false;
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    struct kevent change;
    EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_CLEAR | EV_ONESHOT, 0, 0, ev_ctx);
    if (kevent(reactor->impl.kq_fd, &change, 1, NULL, 0, NULL) == 0) {
        return true;
    }
    free_reactor_ctx(reactor, ev_ctx);
    return false;
#endif
    free_reactor_ctx(reactor, ev_ctx);
    return false;
}

bool cwist_reactor_del(cwist_reactor_t *reactor, int fd) {
    if (!reactor || fd < 0) return false;

#ifdef __linux__
    if (!reactor->impl.use_epoll) {
        struct io_uring_sqe sqe;
        memset(&sqe, 0, sizeof(sqe));
        sqe.opcode = IORING_OP_ASYNC_CANCEL;
        sqe.addr = (unsigned long)fd;
        sqe.cancel_flags = IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_ALL;
        sqe.user_data = 0;
        return uring_submit(reactor, &sqe);
    } else {
        struct epoll_event ev;
        return epoll_ctl(reactor->impl.epoll_fd, EPOLL_CTL_DEL, fd, &ev) == 0;
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    struct kevent change;
    EV_SET(&change, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    return kevent(reactor->impl.kq_fd, &change, 1, NULL, 0, NULL) == 0;
#else
    return false;
#endif
}

void cwist_reactor_run(cwist_reactor_t *reactor) {
    if (!reactor) return;
    reactor->running = true;

#ifdef __linux__
    if (!reactor->impl.use_epoll) {
        while (reactor->running && atomic_load(&g_cwist_running)) {
            int ret = sys_io_uring_enter(reactor->impl.ring_fd, 0, 1, IORING_ENTER_GETEVENTS, NULL);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }
            uint32_t head = __atomic_load_n(reactor->impl.cq_head, __ATOMIC_ACQUIRE);
            uint32_t tail = *reactor->impl.cq_tail;
            if (head == tail) {
                continue;
            }
            while (head != tail) {
                struct io_uring_cqe *cqe = &reactor->impl.cqes[head & *reactor->impl.cq_ring_mask];
                reactor_event_ctx_t *ev_ctx = (reactor_event_ctx_t *)cqe->user_data;
                if (ev_ctx) {
                    if (cqe->res >= 0) {
                        ev_ctx->cb(ev_ctx->fd, ev_ctx->ctx);
                    }
                    free_reactor_ctx(reactor, ev_ctx);
                }
                head++;
            }
            __atomic_store_n(reactor->impl.cq_head, head, __ATOMIC_RELEASE);
        }
    } else {
        struct epoll_event events[1024];
        while (reactor->running && atomic_load(&g_cwist_running)) {
            int n = epoll_wait(reactor->impl.epoll_fd, events, 1024, 100);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < n; i++) {
                reactor_event_ctx_t *ev_ctx = (reactor_event_ctx_t *)events[i].data.ptr;
                if (ev_ctx) {
                    ev_ctx->cb(ev_ctx->fd, ev_ctx->ctx);
                    free_reactor_ctx(reactor, ev_ctx);
                }
            }
        }
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    struct kevent events[1024];
    while (reactor->running && atomic_load(&g_cwist_running)) {
        int n = kevent(reactor->impl.kq_fd, NULL, 0, events, 1024, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            reactor_event_ctx_t *ev_ctx = (reactor_event_ctx_t *)events[i].udata;
            if (ev_ctx) {
                ev_ctx->cb(ev_ctx->fd, ev_ctx->ctx);
                free_reactor_ctx(reactor, ev_ctx);
            }
        }
    }
#endif
}

void cwist_reactor_stop(cwist_reactor_t *reactor) {
    if (reactor) reactor->running = false;
}
