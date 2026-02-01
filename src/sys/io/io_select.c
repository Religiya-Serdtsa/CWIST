#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/io/cwist_io.h>
#include <cwist/core/mem/alloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

// Fallback Implementation using a simple Thread Pool or Main Loop
// This file is used if IO_SRC is set to this in Makefile

struct cwist_io_queue {
    size_t capacity;
    // Simple mock structure
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool running;
};

cwist_io_queue *cwist_io_queue_create(size_t capacity) {
    cwist_io_queue *q = cwist_alloc(sizeof(cwist_io_queue));
    q->capacity = capacity;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
    q->running = true;
    return q;
}

bool cwist_io_queue_submit(cwist_io_queue *q, cwist_job_func func, void *arg) {
    // In fallback mode, we just run it synchronously for now, or spawn a detached thread
    // For simplicity: Synchronous
    if(func) func(arg);
    return true;
}

void cwist_io_queue_run(cwist_io_queue *q) {
    // Dummy loop
    while(q->running) {
        sleep(1);
    }
}

void cwist_io_queue_destroy(cwist_io_queue *q) {
    if(!q) return;
    q->running = false;
    cwist_free(q);
}
