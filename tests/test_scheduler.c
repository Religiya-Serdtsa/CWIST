#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/job/scheduler.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_counter = 0;

static void increment_job(void *arg) {
    int value = *(int *)arg;
    pthread_mutex_lock(&g_mutex);
    g_counter += value;
    pthread_mutex_unlock(&g_mutex);
    free(arg);
}

static void *alloc_arg(int value) {
    int *p = malloc(sizeof(int));
    *p = value;
    return p;
}

int main(void) {
    cwist_scheduler_t *s = cwist_scheduler_create(2, 16);
    assert(s != NULL);

    assert(cwist_scheduler_submit(s, increment_job, alloc_arg(1)));
    assert(cwist_scheduler_submit(s, increment_job, alloc_arg(2)));
    assert(cwist_scheduler_submit(s, increment_job, alloc_arg(3)));

    /* Wait for immediate jobs to drain. */
    for (int i = 0; i < 50; i++) {
        pthread_mutex_lock(&g_mutex);
        int c = g_counter;
        pthread_mutex_unlock(&g_mutex);
        if (c == 6) break;
        usleep(10000);
    }

    pthread_mutex_lock(&g_mutex);
    assert(g_counter == 6);
    pthread_mutex_unlock(&g_mutex);

    /* Schedule a delayed job. */
    assert(cwist_scheduler_schedule(s, increment_job, alloc_arg(10), 150));
    assert(cwist_scheduler_pending_count(s) == 1);

    usleep(300000);

    pthread_mutex_lock(&g_mutex);
    assert(g_counter == 16);
    pthread_mutex_unlock(&g_mutex);
    assert(cwist_scheduler_pending_count(s) == 0);

    cwist_scheduler_destroy(s);
    printf("All scheduler tests passed.\n");
    return 0;
}
