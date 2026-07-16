/**
 * @file test_seq.c
 * @brief Unit tests for the CWIST sequenced-message protocol.
 */

#include <cwist/core/seq/seq.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

static void test_split_and_reassemble(void) {
    printf("Testing seq split/reassemble...\n");
    const char *msg = "1234567890";
    cwist_seq_message_t split;
    assert(cwist_seq_split((const uint8_t *)msg, strlen(msg), 3, &split));
    assert(split.count == 4);

    cwist_seq_assembler_t *a = cwist_seq_assembler_create();
    assert(a != NULL);

    /* Feed out of order. */
    cwist_seq_chunk_t chunk;
    assert(cwist_seq_chunk_parse(split.chunks[2], split.chunk_lens[2], &chunk));
    assert(cwist_seq_assembler_feed(a, &chunk));
    assert(!cwist_seq_assembler_is_complete(a));

    assert(cwist_seq_chunk_parse(split.chunks[0], split.chunk_lens[0], &chunk));
    assert(cwist_seq_assembler_feed(a, &chunk));
    assert(!cwist_seq_assembler_is_complete(a));

    assert(cwist_seq_chunk_parse(split.chunks[3], split.chunk_lens[3], &chunk));
    assert(cwist_seq_assembler_feed(a, &chunk));
    assert(!cwist_seq_assembler_is_complete(a));

    assert(cwist_seq_chunk_parse(split.chunks[1], split.chunk_lens[1], &chunk));
    assert(cwist_seq_assembler_feed(a, &chunk));
    assert(cwist_seq_assembler_is_complete(a));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    assert(cwist_seq_assembler_get_data(a, &out, &out_len));
    assert(out_len == strlen(msg));
    assert(memcmp(out, msg, out_len) == 0);

    cwist_seq_assembler_destroy(a);
    cwist_seq_message_free(&split);
    printf("  Passed split/reassemble.\n");
}

static void test_duplicate_discard(void) {
    printf("Testing duplicate chunk discard...\n");
    const char *msg = "hello world";
    cwist_seq_message_t split;
    assert(cwist_seq_split((const uint8_t *)msg, strlen(msg), 4, &split));

    cwist_seq_assembler_t *a = cwist_seq_assembler_create();
    cwist_seq_chunk_t chunk;
    assert(cwist_seq_chunk_parse(split.chunks[0], split.chunk_lens[0], &chunk));
    assert(cwist_seq_assembler_feed(a, &chunk));
    /* Feed the same chunk again: should be ignored. */
    assert(cwist_seq_assembler_feed(a, &chunk));

    assert(cwist_seq_chunk_parse(split.chunks[1], split.chunk_lens[1], &chunk));
    assert(cwist_seq_assembler_feed(a, &chunk));
    assert(cwist_seq_chunk_parse(split.chunks[2], split.chunk_lens[2], &chunk));
    assert(cwist_seq_assembler_feed(a, &chunk));
    assert(cwist_seq_assembler_is_complete(a));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    assert(cwist_seq_assembler_get_data(a, &out, &out_len));
    assert(out_len == strlen(msg));
    assert(memcmp(out, msg, out_len) == 0);

    cwist_seq_assembler_destroy(a);
    cwist_seq_message_free(&split);
    printf("  Passed duplicate discard.\n");
}

static void test_invalid_chunk(void) {
    printf("Testing invalid sequence pair discard...\n");
    uint8_t bad[CWIST_SEQ_HEADER_SIZE] = {0};
    cwist_seq_chunk_t chunk;
    assert(!cwist_seq_chunk_parse(bad, sizeof(bad), &chunk));

    uint8_t dup_seq[CWIST_SEQ_HEADER_SIZE] = {0, 1, 0, 3, 0, 1, 0, 1};
    /* seq=1 total=3 payload_len=1 chunk_size=1: valid header, but payload missing. */
    assert(!cwist_seq_chunk_parse(dup_seq, sizeof(dup_seq), &chunk));

    uint8_t oversize[CWIST_SEQ_HEADER_SIZE] = {0, 1, 0, 1, 0, 5, 0, 3};
    /* payload_len > chunk_size: invalid. */
    assert(!cwist_seq_chunk_parse(oversize, sizeof(oversize), &chunk));
    printf("  Passed invalid chunk discard.\n");
}

static void test_single_chunk(void) {
    printf("Testing single-chunk message...\n");
    const char *msg = "tiny";
    cwist_seq_message_t split;
    assert(cwist_seq_split((const uint8_t *)msg, strlen(msg), 1024, &split));
    assert(split.count == 1);

    cwist_seq_chunk_t chunk;
    assert(cwist_seq_chunk_parse(split.chunks[0], split.chunk_lens[0], &chunk));
    assert(chunk.seq == 1 && chunk.total == 1);
    assert(chunk.payload_len == strlen(msg));

    cwist_seq_assembler_t *a = cwist_seq_assembler_create();
    assert(cwist_seq_assembler_feed(a, &chunk));
    assert(cwist_seq_assembler_is_complete(a));

    const uint8_t *out = NULL;
    size_t out_len = 0;
    assert(cwist_seq_assembler_get_data(a, &out, &out_len));
    assert(out_len == strlen(msg));
    assert(memcmp(out, msg, out_len) == 0);

    cwist_seq_assembler_destroy(a);
    cwist_seq_message_free(&split);
    printf("  Passed single chunk.\n");
}

int main(void) {
    test_split_and_reassemble();
    test_duplicate_discard();
    test_invalid_chunk();
    test_single_chunk();
    printf("All seq tests passed!\n");
    return 0;
}
