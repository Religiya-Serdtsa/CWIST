#include <cwist/sys/io/cwist_io.h>
#include <cwist/core/mem/alloc.h>
#include <ttak/mem/epoch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

/**
 * @file io_queue.c
 * @brief Lock-free job queue backing cwist_io_queue. Despite the old file
 * name (io_uring.c), this has nothing to do with Linux io_uring; the
 * io_uring readiness multiplexer lives in reactor.c.
 */

/**
 * Lock-free job queue inspired by libttak's SPMC queue samples.
 * We keep a single dummy node (Michael & Scott queue) so producers
 * never contend on a mutex while enqueuing jobs.
 *
 * Memory reclamation: popped sentinel nodes are never freed immediately.
 * Both push and pop run inside ttak EBR critical sections, and an unlinked
 * node crosses two stages (retire_new -> retire_old -> ttak_epoch_retire)
 * gated on observed advances of the ttak global epoch. A node therefore
 * reaches ttak_epoch_retire only after at least one full epoch boundary
 * since its unlink, so a preempted thread still holding the pointer is
 * guaranteed to be inside a stale-epoch critical section and blocks
 * reclamation until it leaves.
 */
typedef struct job_node {
    cwist_job_func func;
    void *arg;
    struct job_node *_Atomic next;
    struct job_node *retire_next;
} job_node_t;

struct cwist_io_queue {
    job_node_t *_Atomic head;
    job_node_t *_Atomic tail;
    atomic_size_t pending_jobs;
    atomic_bool running;
    pthread_mutex_t sleep_lock;
    pthread_cond_t sleep_cond;
    /* Deferred reclamation staging (consumers only, see file banner). */
    pthread_mutex_t retire_lock;
    job_node_t *retire_new;
    job_node_t *retire_old;
    unsigned int last_epoch;
};

/**
 * @brief Allocate one node that stores a queued callback and its argument.
 * @param func Callback to execute.
 * @param arg User payload forwarded to @p func.
 * @return Heap-allocated queue node, or NULL on allocation failure.
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
 * @brief Release a queue node after it is no longer reachable from the list.
 * @param node Node to destroy.
 */
static void cwist_job_node_destroy(job_node_t *node) {
    if (!node) return;
    cwist_free(node);
}

/**
 * @brief Stage an unlinked node for epoch-deferred reclamation.
 * Must be called by a consumer while the node is no longer reachable from
 * the queue; the node is handed to ttak_epoch_retire only after at least
 * one global epoch boundary has passed since its unlink.
 */
static void cwist_queue_defer_free(cwist_io_queue *q, job_node_t *node) {
    pthread_mutex_lock(&q->retire_lock);
    node->retire_next = q->retire_new;
    q->retire_new = node;
    pthread_mutex_unlock(&q->retire_lock);
}

static void cwist_job_node_free_cb(void *ptr) {
    cwist_free(ptr);
}

/**
 * @brief Advance the retirement pipeline and attempt ttak reclamation.
 * On each observed global-epoch advance the staging lists shift by one
 * stage; nodes leaving the oldest stage are retired to ttak, which frees
 * them once no thread sits inside an older epoch.
 */
static void cwist_queue_reclaim(cwist_io_queue *q) {
    ttak_epoch_reclaim();
    unsigned int epoch = atomic_load_explicit(&g_epoch_mgr.global_epoch, memory_order_acquire);

    pthread_mutex_lock(&q->retire_lock);
    if (epoch != q->last_epoch) {
        q->last_epoch = epoch;
        job_node_t *old = q->retire_old;
        q->retire_old = q->retire_new;
        q->retire_new = NULL;
        pthread_mutex_unlock(&q->retire_lock);

        while (old) {
            job_node_t *next = old->retire_next;
            ttak_epoch_retire(old, cwist_job_node_free_cb);
            old = next;
        }
    } else {
        pthread_mutex_unlock(&q->retire_lock);
    }
}

/**
 * @brief Append a node to the lock-free queue and wake sleepers when needed.
 * @param q Queue that receives the node.
 * @param node Node to append.
 */
static void cwist_queue_push(cwist_io_queue *q, job_node_t *node) {
    atomic_store_explicit(&node->next, NULL, memory_order_relaxed);
    ttak_epoch_enter();
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
    ttak_epoch_exit();

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
    job_node_t *result = NULL;
    ttak_epoch_enter();
    while (1) {
        job_node_t *head = atomic_load_explicit(&q->head, memory_order_acquire);
        job_node_t *tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        job_node_t *next = atomic_load_explicit(&head->next, memory_order_acquire);
        if (!next) {
            break;
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
            /* head is now unreachable from the queue, but a preempted
             * producer may still hold it; defer the free across an epoch
             * boundary instead of releasing it here. */
            cwist_queue_defer_free(q, head);
            result = next;
            break;
        }
    }
    ttak_epoch_exit();
    return result;
}

/**
 * @brief Wait until new work arrives or shutdown has been requested.
 * @param q Queue whose sleeper condition should be observed.
 * @return true when work may still arrive, false when the queue should stop.
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
 * @brief Allocate the queue object and install its sentinel node.
 * @param capacity Capacity hint accepted for interface parity and currently unused.
 * @return Queue handle, or NULL when initialization fails.
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
    q->retire_new = NULL;
    q->retire_old = NULL;
    q->last_epoch = atomic_load_explicit(&g_epoch_mgr.global_epoch, memory_order_acquire);

    if (pthread_mutex_init(&q->retire_lock, NULL) != 0) {
        cwist_job_node_destroy(stub);
        cwist_free(q);
        return NULL;
    }
    if (pthread_mutex_init(&q->sleep_lock, NULL) != 0) {
        pthread_mutex_destroy(&q->retire_lock);
        cwist_job_node_destroy(stub);
        cwist_free(q);
        return NULL;
    }
    if (pthread_cond_init(&q->sleep_cond, NULL) != 0) {
        pthread_mutex_destroy(&q->sleep_lock);
        pthread_mutex_destroy(&q->retire_lock);
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
 * @return true on success, or false when the queue is stopping or OOM.
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
 * @brief Process queued jobs until shutdown is requested and no work remains.
 * @param q Queue to run on the current thread.
 */
void cwist_io_queue_run(cwist_io_queue *q) {
    if (!q) return;
    unsigned int since_reclaim = 0;
    while (atomic_load_explicit(&q->running, memory_order_acquire) ||
           atomic_load_explicit(&q->pending_jobs, memory_order_acquire) > 0) {
        job_node_t *node = cwist_queue_pop(q);
        if (!node) {
            cwist_queue_reclaim(q);
            since_reclaim = 0;
            if (!cwist_queue_wait(q)) {
                break;
            }
            continue;
        }

        atomic_fetch_sub_explicit(&q->pending_jobs, 1, memory_order_acq_rel);
        cwist_job_func func = node->func;
        void *arg = node->arg;
        if (func) func(arg);

        // Node becomes the new sentinel and will be released when the next job is popped.
        node->func = NULL;
        node->arg = NULL;

        if (++since_reclaim >= 64) {
            since_reclaim = 0;
            cwist_queue_reclaim(q);
        }
    }
}

/**
 * @brief Request that runners stop without freeing the queue.
 * @param q Queue instance to stop.
 */
void cwist_io_queue_stop(cwist_io_queue *q) {
    if (!q) return;
    atomic_store_explicit(&q->running, false, memory_order_release);
    pthread_mutex_lock(&q->sleep_lock);
    pthread_cond_broadcast(&q->sleep_cond);
    pthread_mutex_unlock(&q->sleep_lock);
}

/**
 * @brief Stop the queue, wake sleepers, and free all remaining nodes.
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

    /* All runners are gone by contract, so staged nodes can be freed
     * directly without an epoch grace period. */
    for (int stage = 0; stage < 2; stage++) {
        job_node_t *list = stage == 0 ? q->retire_new : q->retire_old;
        while (list) {
            job_node_t *next = list->retire_next;
            cwist_job_node_destroy(list);
            list = next;
        }
    }

    pthread_cond_destroy(&q->sleep_cond);
    pthread_mutex_destroy(&q->sleep_lock);
    pthread_mutex_destroy(&q->retire_lock);
    cwist_free(q);
}
