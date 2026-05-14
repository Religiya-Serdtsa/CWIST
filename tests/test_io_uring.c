/**
 * @file test_io_uring.c
 * @brief Smoke tests for the io_uring backend.
 */

#include <cwist/sys/io/io_uring_backend.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

void test_backend_create_destroy(void) {
    printf("test_backend_create_destroy...\n");
    cwist_uring_config_t cfg = CWIST_URING_CONFIG_DEFAULT;
    cfg.sq_entries = 64;
    cfg.cq_entries = 128;
    cfg.use_fixed_buf = true;
    cfg.fixed_buf_count = 8;

    cwist_uring_backend_t *be = cwist_uring_backend_create(&cfg);
    assert(be != NULL);
    cwist_uring_backend_destroy(be);
    printf("  OK\n");
}

void test_stream_register(void) {
    printf("test_stream_register...\n");
    cwist_uring_config_t cfg = CWIST_URING_CONFIG_DEFAULT;
    cfg.sq_entries = 64;
    cfg.cq_entries = 128;

    cwist_uring_backend_t *be = cwist_uring_backend_create(&cfg);
    assert(be != NULL);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    cwist_core_stream_t *s = cwist_uring_stream_register(be, fd, 1);
    assert(s != NULL);
    assert(s->fd == fd);
    assert(s->protocol == 1);

    cwist_uring_stream_unregister(be, s);
    close(fd);
    cwist_uring_backend_destroy(be);
    printf("  OK\n");
}

void test_fixed_buffers(void) {
    printf("test_fixed_buffers...\n");
    cwist_uring_config_t cfg = CWIST_URING_CONFIG_DEFAULT;
    cfg.sq_entries = 64;
    cfg.cq_entries = 128;
    cfg.use_fixed_buf = true;
    cfg.fixed_buf_size = 4096;
    cfg.fixed_buf_count = 4;

    cwist_uring_backend_t *be = cwist_uring_backend_create(&cfg);
    assert(be != NULL);

    uint16_t bid = 0;
    void *ptr = NULL;
    bool ok = cwist_uring_buf_acquire(be, &bid, &ptr);
    assert(ok == true);
    assert(ptr != NULL);
    assert(bid < 4);

    assert(cwist_uring_active_buffers(be) == 1);

    cwist_uring_buf_release(be, bid);
    assert(cwist_uring_active_buffers(be) == 0);

    cwist_uring_backend_destroy(be);
    printf("  OK\n");
}

void test_submit_recvmsg(void) {
    printf("test_submit_recvmsg...\n");
    cwist_uring_config_t cfg = CWIST_URING_CONFIG_DEFAULT;
    cfg.sq_entries = 64;
    cfg.cq_entries = 128;

    cwist_uring_backend_t *be = cwist_uring_backend_create(&cfg);
    assert(be != NULL);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);

    cwist_core_stream_t *s = cwist_uring_stream_register(be, fd, 1);
    assert(s != NULL);

    struct msghdr msg = {0};
    bool ok = cwist_uring_submit_recvmsg(be, s, &msg);
    assert(ok == true);
    assert(cwist_uring_pending_sqes(be) == 1);

    int flushed = cwist_uring_flush_sq(be);
    assert(flushed == 1);
    assert(cwist_uring_pending_sqes(be) == 0);

    cwist_uring_stream_unregister(be, s);
    close(fd);
    cwist_uring_backend_destroy(be);
    printf("  OK\n");
}

int main(void) {
    test_backend_create_destroy();
    test_stream_register();
    test_fixed_buffers();
    test_submit_recvmsg();
    printf("All io_uring backend tests passed.\n");
    return 0;
}
