#define _POSIX_C_SOURCE 200809L
#include <cwist/net/grpc/grpc.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "Check failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

static int fail_append;
static int allocation_failures;
static int disposed_json;
static void *body_pointer;
static size_t failed_size;

static cwist_error_t test_make_error(cwist_errtype_t type) {
    cwist_error_t err = { .errtype = type };
    /* Only the producer's active field has meaning. */
    err.error.err_i16 = fail_append ? 0 : 17;
    return err;
}

static void *test_realloc(void *pointer, size_t size) {
    if (fail_append && pointer == body_pointer && size == failed_size) {
        allocation_failures++;
        return NULL;
    }
    return cwist_realloc(pointer, size);
}

#define make_error test_make_error
#define cwist_realloc test_realloc
#include "../src/core/sstring/sstring.c"
#undef cwist_realloc
#undef make_error

static void test_dispose(cwist_error_t *err) {
    if (err && err->errtype == CWIST_ERR_JSON && err->error.err_json) {
        disposed_json++;
    }
    cwist_error_dispose(err);
}

#define cwist_error_dispose test_dispose
#include "../src/net/grpc/grpc.c"
#undef cwist_error_dispose

static void check_success(void) {
    fail_append = 0;
    cwist_sstring *body = cwist_sstring_create();
    REQUIRE(body != NULL);
    cwist_http_response response = { .body = body };
    cwist_grpc_stream stream = { .res = &response, .status = CWIST_GRPC_OK };
    const unsigned char payload[] = { 'a', 0, 'b' };
    const unsigned char expected[] = {
        0, 0, 0, 0, 3, 'a', 0, 'b',
        0, 0, 0, 0, 1, 'z',
        0, 0, 0, 0, 0
    };
    int rc = cwist_grpc_stream_send(&stream, payload, sizeof(payload));
    REQUIRE(rc == 0);
    REQUIRE(cwist_grpc_stream_send(&stream, "z", 1) == 0);
    REQUIRE(cwist_grpc_stream_send(&stream, NULL, 0) == 0);
    REQUIRE(stream.status == CWIST_GRPC_OK);
    REQUIRE(body->size == sizeof(expected));
    REQUIRE(memcmp(body->data, expected, sizeof(expected)) == 0);
    cwist_sstring_destroy(body);
}

static void check_append_failure(void) {
    fail_append = 0;
    allocation_failures = 0;
    disposed_json = 0;
    cwist_sstring *body = cwist_sstring_create();
    REQUIRE(body != NULL);
    cwist_error_t initial = cwist_sstring_append_len(body, "old", 3);
    REQUIRE(cwist_error_is_ok(&initial));
    cwist_http_response response = { .body = body };
    cwist_grpc_stream stream = { .res = &response, .status = CWIST_GRPC_OK };
    body_pointer = body->data;
    failed_size = body->size + 5 + 3 + 1;
    fail_append = 1;
    int result = cwist_grpc_stream_send(&stream, "abc", 3);
    fail_append = 0;
    REQUIRE(allocation_failures == 1);
    REQUIRE(result == -1);
    REQUIRE(stream.status == CWIST_GRPC_INTERNAL);
    REQUIRE(strcmp(stream.status_message, "failed to append gRPC stream message") == 0);
    REQUIRE(body->data == body_pointer);
    REQUIRE(body->size == 3);
    REQUIRE(memcmp(body->data, "old", 3) == 0);
    REQUIRE(disposed_json == 1);
    cwist_sstring_destroy(body);
}

int main(int argc, char **argv) {
    (void)test_dispose;
    if (argc == 2) {
        REQUIRE(strcmp(argv[1], "json") == 0);
        check_append_failure();
    } else {
        REQUIRE(argc == 1);
        check_success();
        check_append_failure();
        check_success();
    }
    puts("gRPC append error checks passed");
    return 0;
}
