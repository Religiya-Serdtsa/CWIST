#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <cwist/core/mem/alloc.h>
#include <cwist/sys/app/big_dumb_reply.h>

typedef struct {
    cwist_bdr_t *bdr;
    const char *path;
    const char *body;
} bdr_worker_args;

static void *bdr_worker(void *opaque) {
    bdr_worker_args *args = opaque;
    for (int i = 0; i < 1000; ++i) {
        size_t len = 0;
        cwist_bdr_put(args->bdr, "GET", args->path, args->body, strlen(args->body) + 1);
        char *copy = cwist_bdr_copy_get(args->bdr, "GET", args->path, &len);
        if (copy) {
            assert(len == strlen(args->body) + 1);
            assert(strcmp(copy, args->body) == 0);
            cwist_free(copy);
        }
    }
    return NULL;
}

int main(void) {
    cwist_bdr_t *bdr = cwist_bdr_create();
    assert(bdr != NULL);

    const char root[] = "root response";
    const char posts[] = "posts response";
    size_t len = 0;

    /* Two matching observations promote each path independently. */
    cwist_bdr_put(bdr, "GET", "/", root, sizeof(root));
    cwist_bdr_put(bdr, "GET", "/", root, sizeof(root));
    cwist_bdr_put(bdr, "GET", "/posts", posts, sizeof(posts));
    cwist_bdr_put(bdr, "GET", "/posts", posts, sizeof(posts));

    char *copy = cwist_bdr_copy_get(bdr, "GET", "/posts", &len);
    assert(copy != NULL);
    assert(len == sizeof(posts));
    assert(memcmp(copy, posts, len) == 0);

    /* A cache update may retire the internal root blob, never this copy. */
    cwist_bdr_put(bdr, "GET", "/", "new root", sizeof("new root"));
    assert(memcmp(copy, posts, len) == 0);
    cwist_free(copy);

    assert(cwist_bdr_copy_get(bdr, "GET", "/missing", &len) == NULL);

    bdr_worker_args left = { bdr, "/left", "left response" };
    bdr_worker_args right = { bdr, "/right", "right response" };
    pthread_t left_thread;
    pthread_t right_thread;
    assert(pthread_create(&left_thread, NULL, bdr_worker, &left) == 0);
    assert(pthread_create(&right_thread, NULL, bdr_worker, &right) == 0);
    assert(pthread_join(left_thread, NULL) == 0);
    assert(pthread_join(right_thread, NULL) == 0);

    cwist_bdr_destroy(bdr);
    puts("test_bdr: OK");
    return 0;
}
