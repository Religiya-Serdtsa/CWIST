#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cwist/sys/io/uring_sqpoll.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

static inline int sys_io_uring_setup(unsigned entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

int cwist_io_uring_init_sqpoll(cwist_sqpoll_ring_t *r, unsigned entries, unsigned sq_thread_idle_ms) {
    if (!r || entries == 0) return -1;
    memset(r, 0, sizeof(*r));

    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    p.flags = IORING_SETUP_SQPOLL;
    p.sq_thread_idle = sq_thread_idle_ms ? sq_thread_idle_ms : 2000;

    int fd = sys_io_uring_setup(entries, &p);
    if (fd < 0) {
        return -1;
    }

    r->ring_fd = fd;
    r->entries = p.sq_entries;

    size_t sq_sz = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
    void *sq_ptr = mmap(NULL, sq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (sq_ptr == MAP_FAILED) {
        close(fd);
        return -1;
    }
    r->sq_ring_sz = sq_sz;

    r->sq_head = (_Atomic uint32_t *)((uint8_t *)sq_ptr + p.sq_off.head);
    r->sq_tail = (_Atomic uint32_t *)((uint8_t *)sq_ptr + p.sq_off.tail);
    r->sq_ring_mask = (uint32_t *)((uint8_t *)sq_ptr + p.sq_off.ring_mask);
    r->sq_array = (uint32_t *)((uint8_t *)sq_ptr + p.sq_off.array);

    size_t sqes_sz = p.sq_entries * sizeof(struct io_uring_sqe);
    void *sqes_ptr = mmap(NULL, sqes_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (sqes_ptr == MAP_FAILED) {
        munmap(sq_ptr, sq_sz);
        close(fd);
        return -1;
    }
    r->sqes_sz = sqes_sz;
    r->sqes = (struct io_uring_sqe *)sqes_ptr;
    r->active = true;

    return 0;
}

bool cwist_io_uring_sqpoll_send(cwist_sqpoll_ring_t *r, int fd, const void *buf, size_t len, uint64_t user_data) {
    if (!r || !r->active) return false;

    uint32_t tail = atomic_load_explicit(r->sq_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(r->sq_head, memory_order_acquire);

    if (tail - head >= r->entries) {
        return false;
    }

    uint32_t idx = tail & (*r->sq_ring_mask);
    struct io_uring_sqe *sqe = &r->sqes[idx];

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_SEND;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)buf;
    sqe->len = (uint32_t)len;
    sqe->flags = 0;
    sqe->user_data = user_data;

    r->sq_array[idx] = idx;
    atomic_store_explicit(r->sq_tail, tail + 1, memory_order_release);
    return true;
}

void cwist_io_uring_destroy_sqpoll(cwist_sqpoll_ring_t *r) {
    if (!r || !r->active) return;
    if (r->sqes && r->sqes_sz) {
        munmap(r->sqes, r->sqes_sz);
    }
    if (r->sq_head && r->sq_ring_sz) {
        munmap((void *)r->sq_head, r->sq_ring_sz);
    }
    if (r->ring_fd >= 0) {
        close(r->ring_fd);
    }
    memset(r, 0, sizeof(*r));
}
#endif
