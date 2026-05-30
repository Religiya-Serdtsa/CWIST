#include <cwist/sys/io/io_uring_backend.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdatomic.h>

void dummy_cb(struct cwist_core_stream *stream, int32_t res, uint32_t flags, void *user_data) {
    (void)stream; (void)res; (void)flags; (void)user_data;
}

void test_two_phase_demolition(void) {
    printf("test_two_phase_demolition...\n");
    cwist_uring_config_t cfg = CWIST_URING_CONFIG_DEFAULT;
    cfg.sq_entries = 64;
    cfg.cq_entries = 128;
    cfg.max_streams = 4;

    cwist_uring_backend_t *be = cwist_uring_backend_create(&cfg);
    assert(be != NULL);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);

    cwist_core_stream_t *s = cwist_uring_stream_register(be, fd, 1);
    assert(s != NULL);
    uint32_t first_idx = s->index;
    uint32_t first_gen = s->generation;
    printf("  Stream 1: index=%u, gen=%u\n", first_idx, first_gen);
    s->recv_cb = dummy_cb;

    /* 1. Submit a NOP (Safe and non-blocking completion) */
    bool ok = cwist_uring_submit_nop(be, s);
    assert(ok == true);
    assert(atomic_load(&s->pending_io_count) == 1);

    /* 2. Unregister while I/O is pending */
    cwist_uring_stream_unregister(be, s);
    assert(s->is_dead == true);

    /* 3. Try to register a new stream - should NOT get the same index because index 0 is zombie */
    int fd2 = socket(AF_INET, SOCK_DGRAM, 0);
    cwist_core_stream_t *s2 = cwist_uring_stream_register(be, fd2, 1);
    assert(s2 != NULL);
    printf("  Stream 2: index=%u, gen=%u\n", s2->index, s2->generation);
    assert(s2->index != first_idx);

    /* 4. Flush and Poll to process the CQE */
    cwist_uring_flush_sq(be);
    
    printf("  Polling for CQE...\n");
    int processed = cwist_uring_poll_cq(be, 1, 1000);
    printf("  Processed %d CQEs\n", processed);
    assert(processed >= 1);

    /* 5. Now that CQE is processed, the first stream should be finalized and index 0 available again */
    cwist_core_stream_t *s3 = cwist_uring_stream_register(be, socket(AF_INET, SOCK_DGRAM, 0), 1);
    assert(s3 != NULL);
    printf("  Stream 3: index=%u, gen=%u\n", s3->index, s3->generation);
    assert(s3->index == first_idx);
    assert(s3->generation != first_gen);

    cwist_uring_backend_destroy(be);
    printf("  OK\n");
}

int main(void) {
    test_two_phase_demolition();
    printf("Two-phase demolition tests passed.\n");
    return 0;
}
