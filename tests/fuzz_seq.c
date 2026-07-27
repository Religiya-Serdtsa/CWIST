/* libFuzzer entry point for malformed, reordered, and authenticated chunks. */
#include <cwist/core/seq/seq.h>
#include <cwist/core/seq/seq_auth.h>

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    cwist_seq_chunk_t chunk;
    cwist_seq_assembler_t *assembler = cwist_seq_assembler_create_limited(10 * 1024 * 1024);
    if (assembler && cwist_seq_chunk_parse(data, size, &chunk)) {
        (void)cwist_seq_assembler_feed(assembler, &chunk);
        (void)cwist_seq_assembler_is_complete(assembler);
    }
    cwist_seq_assembler_destroy(assembler);

    static const uint8_t key[CWIST_SEQ_AUTH_KEY_SIZE] = { 0x5a };
    static const uint8_t session[CWIST_SEQ_AUTH_SESSION_ID_SIZE] = { 0xa5 };
    cwist_seq_auth_context_t *auth = cwist_seq_auth_context_create(key, session, 64);
    if (auth) {
        cwist_seq_auth_metadata_t metadata;
        (void)cwist_seq_auth_unwrap(auth, data, size, &metadata, &chunk);
    }
    cwist_seq_auth_context_destroy(auth);
    return 0;
}
