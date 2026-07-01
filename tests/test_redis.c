#define _POSIX_C_SOURCE 200809L
#include <cwist/net/redis/cwist_redis.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    const char *host = getenv("CWIST_REDIS_HOST") ? getenv("CWIST_REDIS_HOST") : "127.0.0.1";
    int port = getenv("CWIST_REDIS_PORT") ? atoi(getenv("CWIST_REDIS_PORT")) : 6379;

    cwist_redis_t *r = cwist_redis_connect(host, port);
    if (!r) {
        printf("[redis] No Redis server at %s:%d, skipping.\n", host, port);
        return 0;
    }

    char *out = NULL;
    cwist_error_t err;

    err = cwist_redis_set(r, "cwist:test:hello", "world");
    if (err.error.err_i16 != 0) {
        fprintf(stderr, "[redis] SET failed\n");
        cwist_redis_close(r);
        return 1;
    }

    err = cwist_redis_get(r, "cwist:test:hello", &out);
    if (err.error.err_i16 != 0 || !out || strcmp(out, "world") != 0) {
        fprintf(stderr, "[redis] GET mismatch\n");
        cwist_free(out);
        cwist_redis_close(r);
        return 1;
    }
    cwist_free(out);
    out = NULL;

    /* argv API is binary-safe: values containing a NUL must round-trip. */
    const char binary_value[] = { 'a', '\0', 'b' };
    const void *set_args[] = { "SET", "cwist:test:binary", binary_value };
    size_t set_lens[] = { 3, strlen("cwist:test:binary"), sizeof(binary_value) };
    assert(cwist_redis_command_argv(r, 3, set_args, set_lens, NULL, NULL).error.err_i16 == 0);
    const void *get_args[] = { "GET", "cwist:test:binary" };
    size_t get_lens[] = { 3, strlen("cwist:test:binary") };
    size_t binary_len = 0;
    assert(cwist_redis_command_argv(r, 2, get_args, get_lens, &out, &binary_len).error.err_i16 == 0);
    assert(binary_len == sizeof(binary_value) && memcmp(out, binary_value, binary_len) == 0);
    cwist_free(out);
    out = NULL;

    err = cwist_redis_setex(r, "cwist:test:tmp", "value", 60);
    if (err.error.err_i16 != 0) {
        fprintf(stderr, "[redis] SETEX failed\n");
        cwist_redis_close(r);
        return 1;
    }

    err = cwist_redis_del(r, "cwist:test:hello");
    if (err.error.err_i16 != 0) {
        fprintf(stderr, "[redis] DEL failed\n");
        cwist_redis_close(r);
        return 1;
    }

    err = cwist_redis_get(r, "cwist:test:hello", &out);
    if (err.error.err_i16 != 0 || out != NULL) {
        fprintf(stderr, "[redis] GET after DEL should be NULL\n");
        cwist_free(out);
        cwist_redis_close(r);
        return 1;
    }

    cwist_redis_close(r);

    /* Pool smoke test. */
    cwist_redis_pool_t *pool = cwist_redis_pool_create(host, port, 2);
    if (!pool) {
        printf("[redis] Pool creation failed, skipping pool test.\n");
        return 0;
    }

    err = cwist_redis_pool_set(pool, "cwist:test:pool", "ok");
    if (err.error.err_i16 != 0) {
        fprintf(stderr, "[redis] pool SET failed\n");
        cwist_redis_pool_destroy(pool);
        return 1;
    }

    err = cwist_redis_pool_get(pool, "cwist:test:pool", &out);
    if (err.error.err_i16 != 0 || !out || strcmp(out, "ok") != 0) {
        fprintf(stderr, "[redis] pool GET mismatch\n");
        cwist_free(out);
        cwist_redis_pool_destroy(pool);
        return 1;
    }
    cwist_free(out);
    cwist_redis_pool_del(pool, "cwist:test:pool");
    cwist_redis_pool_destroy(pool);

    printf("All Redis tests passed.\n");
    return 0;
}
