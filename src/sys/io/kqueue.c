#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/io/cwist_io.h>
#include <cwist/core/mem/alloc.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

/**
 * @file kqueue.c
 * @brief BSD kqueue backend for the generic CWIST job queue interface.
 */

struct cwist_io_queue {
    int kq_fd;
};

/**
 * @brief Allocate a kqueue-backed IO queue wrapper.
 * @param capacity Capacity hint accepted for interface parity and currently unused.
 * @return Queue handle, or NULL when kqueue setup fails.
 */
cwist_io_queue *cwist_io_queue_create(size_t capacity) {
    (void)capacity;
    cwist_io_queue *q = cwist_alloc(sizeof(cwist_io_queue));
    if (!q) return NULL;

    q->kq_fd = kqueue();
    if (q->kq_fd < 0) {
        perror("kqueue");
        cwist_free(q);
        return NULL;
    }
    return q;
}

/**
 * @brief Tiny heap object used as kevent user data for one queued callback.
 */
struct job_wrapper {
    cwist_job_func func;
    void *arg;
};

/**
 * @brief Enqueue one callback by triggering an EVFILT_USER event.
 * @param q Kqueue-backed queue handle.
 * @param func Callback to execute when the event is consumed.
 * @param arg User payload forwarded to @p func.
 * @return true on success, false when event registration fails.
 */
bool cwist_io_queue_submit(cwist_io_queue *q, cwist_job_func func, void *arg) {
    struct job_wrapper *job = cwist_alloc(sizeof(struct job_wrapper));
    job->func = func;
    job->arg = arg;

    struct kevent kev;
    // We use EVFILT_USER to trigger a custom event.
    // Ident = pointer to job (unique ID)
    EV_SET(&kev, (uintptr_t)job, EVFILT_USER, EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_TRIGGER, 0, job);
    
    if (kevent(q->kq_fd, &kev, 1, NULL, 0, NULL) < 0) {
        perror("kevent submit");
        cwist_free(job);
        return false;
    }
    return true;
}

/**
 * @brief Block on kqueue events and execute queued callbacks forever.
 * @param q Kqueue-backed queue handle.
 */
void cwist_io_queue_run(cwist_io_queue *q) {
    struct kevent events[32];
    while (1) {
        int n = kevent(q->kq_fd, NULL, 0, events, 32, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("kevent wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].filter == EVFILT_USER) {
                struct job_wrapper *job = (struct job_wrapper *)events[i].udata;
                if (job) {
                    job->func(job->arg);
                    cwist_free(job);
                }
            }
        }
    }
}

/**
 * @brief Close the underlying kqueue descriptor and free the queue wrapper.
 * @param q Queue handle to destroy.
 */
void cwist_io_queue_destroy(cwist_io_queue *q) {
    if (q) {
        close(q->kq_fd);
        cwist_free(q);
    }
}
