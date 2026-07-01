/**
 * @file scheduler.c
 * @brief Background job scheduler with delayed execution.
 *
 * The scheduler is built on top of cwist_io_queue. Worker threads run the
 * queue's event loop, and a dedicated timer thread maintains a min-heap of
 * delayed jobs. When a delayed deadline arrives, the timer thread submits the
 * job to the io queue so a worker can execute it.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/job/scheduler.h>
#include <cwist/core/mem/alloc.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define CWIST_SCHEDULER_HEAP_INIT 16

typedef struct {
    uint64_t deadline_ms;
    cwist_job_func func;
    void *arg;
} scheduled_job_t;

struct cwist_scheduler {
    cwist_io_queue *queue;
    pthread_t *workers;
    size_t worker_count;
    pthread_t timer_thread;

    scheduled_job_t *heap;
    size_t heap_count;
    size_t heap_capacity;

    pthread_mutex_t delayed_mtx;
    pthread_cond_t delayed_cond;
    bool running;
};

/* --- Time helpers ------------------------------------------------------- */

static uint64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void ms_to_timespec(uint64_t ms, struct timespec *ts) {
    ts->tv_sec = (time_t)(ms / 1000ULL);
    ts->tv_nsec = (long)((ms % 1000ULL) * 1000000ULL);
}

static void cond_init_monotonic(pthread_cond_t *cond) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
}

/* --- Min-heap of delayed jobs ------------------------------------------- */

static int heap_grow(cwist_scheduler_t *s) {
    if (s->heap_count < s->heap_capacity) return 0;
    size_t new_cap = s->heap_capacity ? s->heap_capacity * 2 : CWIST_SCHEDULER_HEAP_INIT;
    scheduled_job_t *new_heap = cwist_realloc(s->heap, new_cap * sizeof(scheduled_job_t));
    if (!new_heap) return -1;
    s->heap = new_heap;
    s->heap_capacity = new_cap;
    return 0;
}

static void heap_swap(scheduled_job_t *a, scheduled_job_t *b) {
    scheduled_job_t tmp = *a;
    *a = *b;
    *b = tmp;
}

static void heap_push(cwist_scheduler_t *s, scheduled_job_t job) {
    if (heap_grow(s) != 0) return;
    size_t i = s->heap_count++;
    s->heap[i] = job;
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (s->heap[parent].deadline_ms <= s->heap[i].deadline_ms) break;
        heap_swap(&s->heap[parent], &s->heap[i]);
        i = parent;
    }
}

static scheduled_job_t heap_pop(cwist_scheduler_t *s) {
    scheduled_job_t root = s->heap[0];
    s->heap[0] = s->heap[--s->heap_count];
    size_t i = 0;
    while (1) {
        size_t left = i * 2 + 1;
        size_t right = left + 1;
        size_t smallest = i;
        if (left < s->heap_count && s->heap[left].deadline_ms < s->heap[smallest].deadline_ms) {
            smallest = left;
        }
        if (right < s->heap_count && s->heap[right].deadline_ms < s->heap[smallest].deadline_ms) {
            smallest = right;
        }
        if (smallest == i) break;
        heap_swap(&s->heap[i], &s->heap[smallest]);
        i = smallest;
    }
    return root;
}

/* --- Worker and timer threads ------------------------------------------- */

static void *scheduler_worker_thread(void *arg) {
    cwist_scheduler_t *s = (cwist_scheduler_t *)arg;
    cwist_io_queue_run(s->queue);
    return NULL;
}

static void *scheduler_timer_thread(void *arg) {
    cwist_scheduler_t *s = (cwist_scheduler_t *)arg;

    pthread_mutex_lock(&s->delayed_mtx);
    while (s->running) {
        if (s->heap_count == 0) {
            pthread_cond_wait(&s->delayed_cond, &s->delayed_mtx);
            continue;
        }

        uint64_t current_ms = now_ms();
        if (s->heap[0].deadline_ms <= current_ms) {
            scheduled_job_t job = heap_pop(s);
            pthread_mutex_unlock(&s->delayed_mtx);
            cwist_io_queue_submit(s->queue, job.func, job.arg);
            pthread_mutex_lock(&s->delayed_mtx);
            continue;
        }

        uint64_t wait_ms = s->heap[0].deadline_ms - current_ms;
        struct timespec deadline;
        ms_to_timespec(current_ms + wait_ms, &deadline);
        pthread_cond_timedwait(&s->delayed_cond, &s->delayed_mtx, &deadline);
    }
    pthread_mutex_unlock(&s->delayed_mtx);
    return NULL;
}

static void scheduler_cleanup_threads(cwist_scheduler_t *s, size_t started_workers) {
    s->running = false;
    cwist_io_queue_stop(s->queue);
    for (size_t i = 0; i < started_workers; i++) {
        pthread_join(s->workers[i], NULL);
    }
}

/* --- Public API --------------------------------------------------------- */

cwist_scheduler_t *cwist_scheduler_create(size_t worker_count, size_t queue_capacity) {
    if (worker_count == 0) worker_count = 1;

    cwist_scheduler_t *s = (cwist_scheduler_t *)cwist_alloc(sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));

    s->queue = cwist_io_queue_create(queue_capacity);
    if (!s->queue) {
        cwist_free(s);
        return NULL;
    }

    s->workers = (pthread_t *)cwist_alloc_array(worker_count, sizeof(pthread_t));
    if (!s->workers) {
        cwist_io_queue_destroy(s->queue);
        cwist_free(s);
        return NULL;
    }
    s->worker_count = worker_count;

    pthread_mutex_init(&s->delayed_mtx, NULL);
    cond_init_monotonic(&s->delayed_cond);
    s->running = true;

    for (size_t i = 0; i < worker_count; i++) {
        if (pthread_create(&s->workers[i], NULL, scheduler_worker_thread, s) != 0) {
            scheduler_cleanup_threads(s, i);
            pthread_mutex_destroy(&s->delayed_mtx);
            pthread_cond_destroy(&s->delayed_cond);
            cwist_free(s->workers);
            cwist_io_queue_destroy(s->queue);
            cwist_free(s);
            return NULL;
        }
    }

    if (pthread_create(&s->timer_thread, NULL, scheduler_timer_thread, s) != 0) {
        scheduler_cleanup_threads(s, worker_count);
        pthread_mutex_destroy(&s->delayed_mtx);
        pthread_cond_destroy(&s->delayed_cond);
        cwist_free(s->workers);
        cwist_io_queue_destroy(s->queue);
        cwist_free(s);
        return NULL;
    }

    return s;
}

void cwist_scheduler_destroy(cwist_scheduler_t *s) {
    if (!s) return;

    s->running = false;
    pthread_mutex_lock(&s->delayed_mtx);
    pthread_cond_broadcast(&s->delayed_cond);
    pthread_mutex_unlock(&s->delayed_mtx);

    cwist_io_queue_stop(s->queue);

    for (size_t i = 0; i < s->worker_count; i++) {
        pthread_join(s->workers[i], NULL);
    }
    pthread_join(s->timer_thread, NULL);

    cwist_io_queue_destroy(s->queue);
    cwist_free(s->heap);
    cwist_free(s->workers);
    pthread_mutex_destroy(&s->delayed_mtx);
    pthread_cond_destroy(&s->delayed_cond);
    cwist_free(s);
}

bool cwist_scheduler_submit(cwist_scheduler_t *s, cwist_job_func func, void *arg) {
    if (!s || !func || !s->running) return false;
    return cwist_io_queue_submit(s->queue, func, arg);
}

bool cwist_scheduler_schedule(cwist_scheduler_t *s, cwist_job_func func, void *arg,
                              uint64_t delay_ms) {
    if (!s || !func || !s->running) return false;
    if (delay_ms == 0) {
        return cwist_io_queue_submit(s->queue, func, arg);
    }

    scheduled_job_t job;
    job.deadline_ms = now_ms() + delay_ms;
    job.func = func;
    job.arg = arg;

    pthread_mutex_lock(&s->delayed_mtx);
    size_t before = s->heap_count;
    heap_push(s, job);
    if (s->heap_count != before + 1) {
        pthread_mutex_unlock(&s->delayed_mtx);
        return false;
    }
    pthread_cond_signal(&s->delayed_cond);
    pthread_mutex_unlock(&s->delayed_mtx);
    return true;
}

size_t cwist_scheduler_pending_count(cwist_scheduler_t *s) {
    if (!s) return 0;
    pthread_mutex_lock(&s->delayed_mtx);
    size_t count = s->heap_count;
    pthread_mutex_unlock(&s->delayed_mtx);
    return count;
}
