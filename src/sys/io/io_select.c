#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/io/cwist_io.h>
#include <cwist/core/mem/alloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

/**
 * @file io_select.c
 * @brief Lock-free job queue fallback used when no platform-specific async backend is selected.
 */

typedef struct job_node {
    cwist_job_func func;
    void *arg;
    _Atomic(struct job_node *) next;
} job_node_t;

struct cwist_io_queue {
    _Atomic(job_node_t *) head;
    _Atomic(job_node_t *) tail;
    atomic_size_t pending_jobs;
    atomic_bool running;
    pthread_mutex_t sleep_lock;
    pthread_cond_t sleep_cond;
};

/**
 * @brief Allocate a queue node for a submitted job.
 * @param func Callback to execute.
 * @param arg User data forwarded to @p func.
 * @return Newly allocated node, or NULL when allocation fails.
 */
static job_node_t *cwist_job_node_create(cwist_job_func func, void *arg) {
    job_node_t *node = cwist_alloc(sizeof(*node));
    if (!node) return NULL;
    node->func = func;
    node->arg = arg;
    atomic_store_explicit(&node->next, NULL, memory_order_relaxed);
    return node;
}

/**
 * @brief Release a queue node previously allocated for a job or sentinel.
 * @param node Node to destroy.
 */
static void cwist_job_node_destroy(job_node_t *node) {
    if (!node) return;
    cwist_free(node);
}

/**
 * @brief Push a node onto the Michael-Scott style linked queue.
 * @param q Queue receiving the new job.
 * @param node Node to append.
 */
static void cwist_queue_push(cwist_io_queue *q, job_node_t *node) {
    atomic_store_explicit(&node->next, NULL, memory_order_relaxed);
    while (1) {
        job_node_t *tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        job_node_t *next = atomic_load_explicit(&tail->next, memory_order_acquire);
        if (tail == atomic_load_explicit(&q->tail, memory_order_acquire)) {
            if (!next) {
                if (atomic_compare_exchange_weak_explicit(&tail->next, &next, node,
                                                          memory_order_release,
                                                          memory_order_relaxed)) {
                    atomic_compare_exchange_strong_explicit(&q->tail, &tail, node,
                                                            memory_order_release,
                                                            memory_order_relaxed);
                    break;
                }
            } else {
                atomic_compare_exchange_weak_explicit(&q->tail, &tail, next,
                                                      memory_order_release,
                                                      memory_order_relaxed);
            }
        }
    }

    size_t prev = atomic_fetch_add_explicit(&q->pending_jobs, 1, memory_order_release);
    if (prev == 0) {
        pthread_mutex_lock(&q->sleep_lock);
        pthread_cond_signal(&q->sleep_cond);
        pthread_mutex_unlock(&q->sleep_lock);
    }
}

/**
 * @brief Pop the next executable node from the queue.
 * @param q Queue to consume from.
 * @return Next job node, or NULL when the queue is empty.
 */
static job_node_t *cwist_queue_pop(cwist_io_queue *q) {
    while (1) {
        job_node_t *head = atomic_load_explicit(&q->head, memory_order_acquire);
        job_node_t *tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        job_node_t *next = atomic_load_explicit(&head->next, memory_order_acquire);
        if (!next) {
            return NULL;
        }
        if (head == tail) {
            atomic_compare_exchange_weak_explicit(&q->tail, &tail, next,
                                                  memory_order_release,
                                                  memory_order_relaxed);
            continue;
        }
        if (atomic_compare_exchange_weak_explicit(&q->head, &head, next,
                                                  memory_order_acq_rel,
                                                  memory_order_relaxed)) {
            cwist_job_node_destroy(head);
            return next;
        }
    }
}

/**
 * @brief Sleep until work arrives or shutdown is requested.
 * @param q Queue whose condition variable should be observed.
 * @return true when work may still arrive, false when the queue is stopping and empty.
 */
static bool cwist_queue_wait(cwist_io_queue *q) {
    pthread_mutex_lock(&q->sleep_lock);
    while (atomic_load_explicit(&q->pending_jobs, memory_order_acquire) == 0 &&
           atomic_load_explicit(&q->running, memory_order_acquire)) {
        pthread_cond_wait(&q->sleep_cond, &q->sleep_lock);
    }
    bool should_continue =
        atomic_load_explicit(&q->running, memory_order_acquire) ||
        atomic_load_explicit(&q->pending_jobs, memory_order_acquire) > 0;
    pthread_mutex_unlock(&q->sleep_lock);
    return should_continue;
}

/**
 * @brief Allocate a queue instance and install its sentinel node.
 * @param capacity Requested capacity hint. Currently unused by the select fallback.
 * @return Queue handle, or NULL when allocation or synchronization setup fails.
 */
cwist_io_queue *cwist_io_queue_create(size_t capacity) {
    (void)capacity;
    cwist_io_queue *q = cwist_alloc(sizeof(*q));
    if (!q) return NULL;

    job_node_t *stub = cwist_job_node_create(NULL, NULL);
    if (!stub) {
        cwist_free(q);
        return NULL;
    }

    atomic_store_explicit(&q->head, stub, memory_order_relaxed);
    atomic_store_explicit(&q->tail, stub, memory_order_relaxed);
    atomic_init(&q->pending_jobs, 0);
    atomic_store_explicit(&q->running, true, memory_order_release);

    if (pthread_mutex_init(&q->sleep_lock, NULL) != 0) {
        cwist_job_node_destroy(stub);
        cwist_free(q);
        return NULL;
    }
    if (pthread_cond_init(&q->sleep_cond, NULL) != 0) {
        pthread_mutex_destroy(&q->sleep_lock);
        cwist_job_node_destroy(stub);
        cwist_free(q);
        return NULL;
    }
    return q;
}

/**
 * @brief Submit a callback for asynchronous execution by the queue runner.
 * @param q Queue that should own the job.
 * @param func Callback to execute.
 * @param arg User payload forwarded to @p func.
 * @return true on success, or false when the queue is shutting down or OOM.
 */
bool cwist_io_queue_submit(cwist_io_queue *q, cwist_job_func func, void *arg) {
    if (!q || !func) return false;
    if (!atomic_load_explicit(&q->running, memory_order_acquire)) {
        return false;
    }

    job_node_t *node = cwist_job_node_create(func, arg);
    if (!node) return false;
    cwist_queue_push(q, node);
    return true;
}

/**
 * @brief Drain queued jobs until shutdown is requested and no work remains.
 * @param q Queue to run on the current thread.
 */
void cwist_io_queue_run(cwist_io_queue *q) {
    if (!q) return;
    while (atomic_load_explicit(&q->running, memory_order_acquire) ||
           atomic_load_explicit(&q->pending_jobs, memory_order_acquire) > 0) {
        job_node_t *node = cwist_queue_pop(q);
        if (!node) {
            if (!cwist_queue_wait(q)) {
                break;
            }
            continue;
        }

        atomic_fetch_sub_explicit(&q->pending_jobs, 1, memory_order_acq_rel);
        cwist_job_func func = node->func;
        void *arg = node->arg;
        if (func) func(arg);
        node->func = NULL;
        node->arg = NULL;
    }
}

/**
 * @brief Stop the queue, wake sleepers, and free every remaining node.
 * @param q Queue instance to destroy.
 */
void cwist_io_queue_destroy(cwist_io_queue *q) {
    if (!q) return;
    atomic_store_explicit(&q->running, false, memory_order_release);
    pthread_mutex_lock(&q->sleep_lock);
    pthread_cond_broadcast(&q->sleep_cond);
    pthread_mutex_unlock(&q->sleep_lock);

    job_node_t *node = atomic_load_explicit(&q->head, memory_order_acquire);
    while (node) {
        job_node_t *next = atomic_load_explicit(&node->next, memory_order_acquire);
        cwist_job_node_destroy(node);
        node = next;
    }

    pthread_cond_destroy(&q->sleep_cond);
    pthread_mutex_destroy(&q->sleep_lock);
    cwist_free(q);
}
