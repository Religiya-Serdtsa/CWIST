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

typedef struct {
    int ring_fd;
    struct io_uring_sqe *sqes;
    struct io_uring_cqe *cqes;
    uint32_t *sq_head, *sq_tail, *sq_ring_mask, *sq_array;
    uint32_t *cq_head, *cq_tail, *cq_ring_mask;
    uint32_t sq_entries;
    uint32_t cq_entries;
    size_t sq_ring_sz, cq_ring_sz;
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

struct cwist_reactor {
    reactor_impl_t impl;
    bool running;
};

typedef struct {
    int fd;
    cwist_reactor_cb_t cb;
    void *ctx;
} reactor_event_ctx_t;

cwist_reactor_t *cwist_reactor_create(void) {
    cwist_reactor_t *r = cwist_alloc(sizeof(cwist_reactor_t));
    if (!r) return NULL;
    r->running = false;

#ifdef __linux__
    r->impl.use_epoll = false;
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    int fd = sys_io_uring_setup(4096, &p);
    if (fd >= 0) {
        r->impl.ring_fd = fd;
        r->impl.sq_ring_sz = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
        r->impl.cq_ring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
        
        void *sq_ptr = mmap(NULL, r->impl.sq_ring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
        void *cq_ptr = mmap(NULL, r->impl.cq_ring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
        void *sqes_ptr = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
        
        if (sq_ptr != MAP_FAILED && cq_ptr != MAP_FAILED && sqes_ptr != MAP_FAILED) {
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
        } else {
            if (sq_ptr != MAP_FAILED) munmap(sq_ptr, r->impl.sq_ring_sz);
            if (cq_ptr != MAP_FAILED) munmap(cq_ptr, r->impl.cq_ring_sz);
            if (sqes_ptr != MAP_FAILED) munmap(sqes_ptr, p.sq_entries * sizeof(struct io_uring_sqe));
            close(fd);
            r->impl.use_epoll = true;
        }
    } else {
        r->impl.use_epoll = true;
    }
    
    if (r->impl.use_epoll) {
        r->impl.epoll_fd = epoll_create1(0);
        if (r->impl.epoll_fd < 0) {
            cwist_free(r);
            return NULL;
        }
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    r->impl.kq_fd = kqueue();
    if (r->impl.kq_fd < 0) {
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
        close(reactor->impl.ring_fd);
    } else {
        close(reactor->impl.epoll_fd);
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    close(reactor->impl.kq_fd);
#endif
    cwist_free(reactor);
}

bool cwist_reactor_add(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb, void *ctx) {
    if (!reactor || fd < 0) return false;
    reactor_event_ctx_t *ev_ctx = cwist_alloc(sizeof(reactor_event_ctx_t));
    if (!ev_ctx) return false;
    ev_ctx->fd = fd;
    ev_ctx->cb = cb;
    ev_ctx->ctx = ctx;

#ifdef __linux__
    if (!reactor->impl.use_epoll) {
        uint32_t tail = *reactor->impl.sq_tail;
        uint32_t head = __atomic_load_n(reactor->impl.sq_head, __ATOMIC_ACQUIRE);
        if (tail - head >= reactor->impl.sq_entries) {
            cwist_free(ev_ctx);
            return false;
        }
        uint32_t index = tail & *reactor->impl.sq_ring_mask;
        struct io_uring_sqe *sqe = &reactor->impl.sqes[index];
        memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = IORING_OP_POLL_ADD;
        sqe->fd = fd;
        sqe->poll_events = POLLIN;
        sqe->user_data = (uint64_t)ev_ctx;
        
        reactor->impl.sq_array[index] = index;
        __atomic_store_n(reactor->impl.sq_tail, tail + 1, __ATOMIC_RELEASE);
        if (sys_io_uring_enter(reactor->impl.ring_fd, 1, 0, 0, NULL) < 0) {
            __atomic_store_n(reactor->impl.sq_tail, tail, __ATOMIC_RELEASE);
            cwist_free(ev_ctx);
            return false;
        }
        return true;
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
        cwist_free(ev_ctx);
        return false;
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    struct kevent change;
    EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_CLEAR | EV_ONESHOT, 0, 0, ev_ctx);
    return kevent(reactor->impl.kq_fd, &change, 1, NULL, 0, NULL) == 0;
#endif
    return false;
}

bool cwist_reactor_mod(cwist_reactor_t *reactor, int fd, cwist_reactor_cb_t cb, void *ctx) {
    if (!reactor || fd < 0) return false;
    reactor_event_ctx_t *ev_ctx = cwist_alloc(sizeof(reactor_event_ctx_t));
    if (!ev_ctx) return false;
    ev_ctx->fd = fd;
    ev_ctx->cb = cb;
    ev_ctx->ctx = ctx;

#ifdef __linux__
    if (!reactor->impl.use_epoll) {
        uint32_t tail = *reactor->impl.sq_tail;
        uint32_t head = __atomic_load_n(reactor->impl.sq_head, __ATOMIC_ACQUIRE);
        if (tail - head >= reactor->impl.sq_entries) {
            cwist_free(ev_ctx);
            return false;
        }
        uint32_t index = tail & *reactor->impl.sq_ring_mask;
        struct io_uring_sqe *sqe = &reactor->impl.sqes[index];
        memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = IORING_OP_POLL_ADD;
        sqe->fd = fd;
        sqe->poll_events = POLLIN;
        sqe->user_data = (uint64_t)ev_ctx;
        
        reactor->impl.sq_array[index] = index;
        __atomic_store_n(reactor->impl.sq_tail, tail + 1, __ATOMIC_RELEASE);
        if (sys_io_uring_enter(reactor->impl.ring_fd, 1, 0, 0, NULL) < 0) {
            __atomic_store_n(reactor->impl.sq_tail, tail, __ATOMIC_RELEASE);
            cwist_free(ev_ctx);
            return false;
        }
        return true;
    } else {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        ev.data.ptr = ev_ctx;
        if (epoll_ctl(reactor->impl.epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0) {
            return true;
        }
        cwist_free(ev_ctx);
        return false;
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    struct kevent change;
    EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_CLEAR | EV_ONESHOT, 0, 0, ev_ctx);
    return kevent(reactor->impl.kq_fd, &change, 1, NULL, 0, NULL) == 0;
#endif
    return false;
}

bool cwist_reactor_del(cwist_reactor_t *reactor, int fd) {
    if (!reactor || fd < 0) return false;

#ifdef __linux__
    if (!reactor->impl.use_epoll) {
        /* Cancel any pending poll request for this fd.  The original
         * request's CQE (with -ECANCELED) will free the ev_ctx. */
        uint32_t tail = *reactor->impl.sq_tail;
        uint32_t head = __atomic_load_n(reactor->impl.sq_head, __ATOMIC_ACQUIRE);
        if (tail - head >= reactor->impl.sq_entries) return false;
        uint32_t index = tail & *reactor->impl.sq_ring_mask;
        struct io_uring_sqe *sqe = &reactor->impl.sqes[index];
        memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = IORING_OP_ASYNC_CANCEL;
        sqe->addr = (unsigned long)fd;
        sqe->cancel_flags = IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_ALL;
        sqe->user_data = 0;
        reactor->impl.sq_array[index] = index;
        __atomic_store_n(reactor->impl.sq_tail, tail + 1, __ATOMIC_RELEASE);
        if (sys_io_uring_enter(reactor->impl.ring_fd, 1, 0, 0, NULL) < 0) {
            __atomic_store_n(reactor->impl.sq_tail, tail, __ATOMIC_RELEASE);
            return false;
        }
        return true;
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
                /* Spurious wakeup or no event ready. Sleep briefly instead of
                 * tight-looping back into the syscall. */
                usleep(1000);
                continue;
            }
            while (head != tail) {
                struct io_uring_cqe *cqe = &reactor->impl.cqes[head & *reactor->impl.cq_ring_mask];
                reactor_event_ctx_t *ev_ctx = (reactor_event_ctx_t *)cqe->user_data;
                if (ev_ctx) {
                    if (cqe->res >= 0) {
                        ev_ctx->cb(ev_ctx->fd, ev_ctx->ctx);
                    }
                    cwist_free(ev_ctx);
                }
                head++;
            }
            __atomic_store_n(reactor->impl.cq_head, head, __ATOMIC_RELEASE);
        }
    } else {
        struct epoll_event events[1024];
        while (reactor->running && atomic_load(&g_cwist_running)) {
            int n = epoll_wait(reactor->impl.epoll_fd, events, 1024, -1);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            /* Dispatch listening/accept events first, then active sockets in LRU order */
            for (int i = 0; i < n; i++) {
                reactor_event_ctx_t *ev_ctx = (reactor_event_ctx_t *)events[i].data.ptr;
                if (ev_ctx) {
                    ev_ctx->cb(ev_ctx->fd, ev_ctx->ctx);
                    cwist_free(ev_ctx);
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
                cwist_free(ev_ctx);
            }
        }
    }
#endif
}

void cwist_reactor_stop(cwist_reactor_t *reactor) {
    if (reactor) reactor->running = false;
}
