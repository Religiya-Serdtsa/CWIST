#include <cwist/sys/io/cwist_io.h>
#include <cwist/core/mem/alloc.h>
#include <pthread.h>

struct job_wrapper {
    cwist_job_func func;
    void *arg;
    struct job_wrapper *next;
};

struct cwist_io_queue {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct job_wrapper *head;
    struct job_wrapper *tail;
};

cwist_io_queue *cwist_io_queue_create(size_t capacity) {
    (void)capacity;
    cwist_io_queue *q = cwist_alloc(sizeof(*q));
    if (!q) return NULL;
    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        cwist_free(q);
        return NULL;
    }
    if (pthread_cond_init(&q->cond, NULL) != 0) {
        pthread_mutex_destroy(&q->lock);
        cwist_free(q);
        return NULL;
    }
    q->head = NULL;
    q->tail = NULL;
    return q;
}

bool cwist_io_queue_submit(cwist_io_queue *q, cwist_job_func func, void *arg) {
    if (!q || !func) return false;
    struct job_wrapper *job = cwist_alloc(sizeof(*job));
    if (!job) return false;
    job->func = func;
    job->arg = arg;
    job->next = NULL;

    pthread_mutex_lock(&q->lock);
    if (q->tail) {
        q->tail->next = job;
    } else {
        q->head = job;
    }
    q->tail = job;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
    return true;
}

void cwist_io_queue_run(cwist_io_queue *q) {
    if (!q) return;
    while (1) {
        pthread_mutex_lock(&q->lock);
        while (!q->head) {
            pthread_cond_wait(&q->cond, &q->lock);
        }
        struct job_wrapper *job = q->head;
        q->head = job->next;
        if (!q->head) {
            q->tail = NULL;
        }
        pthread_mutex_unlock(&q->lock);

        job->func(job->arg);
        cwist_free(job);
    }
}

void cwist_io_queue_destroy(cwist_io_queue *q) {
    if (!q) return;
    pthread_mutex_lock(&q->lock);
    struct job_wrapper *job = q->head;
    q->head = NULL;
    q->tail = NULL;
    pthread_mutex_unlock(&q->lock);
    while (job) {
        struct job_wrapper *next = job->next;
        cwist_free(job);
        job = next;
    }
    pthread_cond_destroy(&q->cond);
    pthread_mutex_destroy(&q->lock);
    cwist_free(q);
}
