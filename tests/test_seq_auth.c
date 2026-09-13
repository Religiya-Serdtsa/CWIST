#include <cwist/core/seq/seq_auth.h>
#include <cwist/core/mem/alloc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    uint8_t key[CWIST_SEQ_AUTH_KEY_SIZE] = {1};
    uint8_t session[CWIST_SEQ_AUTH_SESSION_ID_SIZE] = {2};
    uint8_t message_id[CWIST_SEQ_AUTH_MESSAGE_ID_SIZE] = {3};
    uint8_t nonce[CWIST_SEQ_AUTH_NONCE_SIZE] = {4};
    cwist_seq_auth_context_t *ctx = cwist_seq_auth_context_create(key, session, 8);
    assert(ctx);
    cwist_seq_message_t split;
    assert(cwist_seq_split((const uint8_t *)"secure", 6, 16, &split));
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    assert(cwist_seq_auth_wrap(ctx, message_id, nonce, split.chunks[0], split.chunk_lens[0],
                               &wire, &wire_len));
    cwist_seq_auth_metadata_t meta;
    cwist_seq_chunk_t chunk;
    assert(cwist_seq_auth_unwrap(ctx, wire, wire_len, &meta, &chunk));
    assert(chunk.payload_len == 6 && memcmp(chunk.payload, "secure", 6) == 0);
    assert(!cwist_seq_auth_unwrap(ctx, wire, wire_len, &meta, &chunk)); /* nonce replay */
    wire[wire_len - 1] ^= 1;
    uint8_t nonce2[CWIST_SEQ_AUTH_NONCE_SIZE] = {5};
    uint8_t *tampered = NULL;
    size_t tampered_len = 0;
    assert(cwist_seq_auth_wrap(ctx, message_id, nonce2, split.chunks[0], split.chunk_lens[0],
                               &tampered, &tampered_len));
    tampered[tampered_len - 1] ^= 1;
    assert(!cwist_seq_auth_unwrap(ctx, tampered, tampered_len, &meta, &chunk));
    cwist_seq_auth_mark_complete(ctx, message_id);
    uint8_t nonce3[CWIST_SEQ_AUTH_NONCE_SIZE] = {6};
    uint8_t *replay = NULL;
    size_t replay_len = 0;
    assert(cwist_seq_auth_wrap(ctx, message_id, nonce3, split.chunks[0], split.chunk_lens[0],
                               &replay, &replay_len));
    assert(!cwist_seq_auth_unwrap(ctx, replay, replay_len, &meta, &chunk));
    cwist_free(wire); cwist_free(tampered); cwist_free(replay);
    cwist_seq_message_free(&split);

    /* Test chunk with total > CWIST_SEQ_MAX_CHUNKS is rejected */
    uint8_t invalid_chunk[CWIST_SEQ_AUTH_HEADER_SIZE + 4];
    memset(invalid_chunk, 0, sizeof(invalid_chunk));
    invalid_chunk[0] = 0; invalid_chunk[1] = 1; /* seq = 1 */
    uint16_t excessive_total = CWIST_SEQ_MAX_CHUNKS + 1;
    invalid_chunk[2] = (uint8_t)(excessive_total >> 8);
    invalid_chunk[3] = (uint8_t)(excessive_total & 0xff);
    invalid_chunk[4] = 0; invalid_chunk[5] = 4; /* payload_len = 4 */
    invalid_chunk[6] = 0; invalid_chunk[7] = 4; /* chunk_size = 4 */
    assert(!cwist_seq_auth_unwrap(ctx, invalid_chunk, sizeof(invalid_chunk), &meta, &chunk));

    cwist_seq_auth_context_destroy(ctx);
    puts("Authenticated sequence tests passed.");
    return 0;
}
