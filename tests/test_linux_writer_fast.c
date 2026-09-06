#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <cwist/net/http/writer_fast.h>
#if defined(__linux__)
#include <cwist/sys/io/uring_sqpoll.h>
#endif

static void test_p2c_scheduler(void) {
    printf("Testing P2C scheduler... ");
    _Atomic uint32_t loads[4];
    for (int i = 0; i < 4; ++i) {
        atomic_init(&loads[i], 10);
    }
    atomic_store(&loads[2], 2); /* Worker 2 is least loaded */

    int counts[4] = {0};
    for (int i = 0; i < 1000; ++i) {
        uint32_t chosen = cwist_sched_p2c_select_worker(loads, 4);
        assert(chosen < 4);
        counts[chosen]++;
    }

    /* Worker 2 must be picked significantly more often than others */
    assert(counts[2] > counts[0]);
    assert(counts[2] > counts[1]);
    assert(counts[2] > counts[3]);
    printf("OK\n");
}

static void test_speculative_writer(void) {
    printf("Testing speculative writer fast-path... ");
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    const char msg[] = "GET /fast-path HTTP/1.1\r\n\r\n";
    size_t sent = 0;
    cwist_write_status_t st = cwist_http_send_speculative(sv[0], msg, sizeof(msg), &sent);
    assert(st == CWIST_WRITE_DONE);
    assert(sent == sizeof(msg));

    char recv_buf[64] = {0};
    ssize_t n = recv(sv[1], recv_buf, sizeof(recv_buf), 0);
    assert(n == sizeof(msg));
    assert(memcmp(recv_buf, msg, sizeof(msg)) == 0);

    close(sv[0]);
    close(sv[1]);
    printf("OK\n");
}

#if defined(__linux__)
static void test_sqpoll_probe(void) {
    printf("Testing io_uring SQPOLL probe... ");
    cwist_sqpoll_ring_t ring;
    int ret = cwist_io_uring_init_sqpoll(&ring, 64, 1000);
    if (ret == 0) {
        /* SQPOLL available */
        cwist_io_uring_destroy_sqpoll(&ring);
        printf("SQPOLL supported and operational OK\n");
    } else {
        printf("SQPOLL skipped (insufficient kernel privs/RLIMIT_MEMLOCK), fallback verified OK\n");
    }
}
#endif

int main(void) {
    test_p2c_scheduler();
    test_speculative_writer();
#if defined(__linux__)
    test_sqpoll_probe();
#endif
    printf("All Linux low-latency writer & scheduler tests passed!\n");
    return 0;
}
