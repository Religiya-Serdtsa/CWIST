/**
 * @file seq_auth.h
 * @brief Authenticated v2 envelope for CWIST sequenced payloads.
 *
 * The legacy eight-byte sequence header has no cryptographic binding.  This
 * opt-in envelope authenticates every fragment with HMAC-SHA-256, binds it to
 * an application session, and keeps bounded nonce/message-id replay windows.
 */
#ifndef __CWIST_CORE_SEQ_SEQ_AUTH_H__
#define __CWIST_CORE_SEQ_SEQ_AUTH_H__

#include <cwist/core/seq/seq.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CWIST_SEQ_AUTH_KEY_SIZE 32
#define CWIST_SEQ_AUTH_SESSION_ID_SIZE 16
#define CWIST_SEQ_AUTH_MESSAGE_ID_SIZE 16
#define CWIST_SEQ_AUTH_NONCE_SIZE 12
#define CWIST_SEQ_AUTH_TAG_SIZE 16
#define CWIST_SEQ_AUTH_HEADER_SIZE \
    (CWIST_SEQ_HEADER_SIZE + CWIST_SEQ_AUTH_MESSAGE_ID_SIZE + \
     CWIST_SEQ_AUTH_NONCE_SIZE + CWIST_SEQ_AUTH_TAG_SIZE)

typedef struct cwist_seq_auth_context cwist_seq_auth_context_t;

typedef struct cwist_seq_auth_metadata {
    uint8_t message_id[CWIST_SEQ_AUTH_MESSAGE_ID_SIZE];
    uint8_t nonce[CWIST_SEQ_AUTH_NONCE_SIZE];
} cwist_seq_auth_metadata_t;

/** Create a context with a per-session key and bounded replay windows. */
cwist_seq_auth_context_t *cwist_seq_auth_context_create(
    const uint8_t key[CWIST_SEQ_AUTH_KEY_SIZE],
    const uint8_t session_id[CWIST_SEQ_AUTH_SESSION_ID_SIZE],
    size_t replay_window);
void cwist_seq_auth_context_destroy(cwist_seq_auth_context_t *ctx);

/** Generate a cryptographically random message ID or fragment nonce. */
bool cwist_seq_auth_random(uint8_t *out, size_t len);

/**
 * Wrap an ordinary cwist_seq_split() chunk in the v2 authenticated envelope.
 * Each retransmission must use a fresh nonce.  The caller owns *out.
 */
bool cwist_seq_auth_wrap(const cwist_seq_auth_context_t *ctx,
                         const uint8_t message_id[CWIST_SEQ_AUTH_MESSAGE_ID_SIZE],
                         const uint8_t nonce[CWIST_SEQ_AUTH_NONCE_SIZE],
                         const uint8_t *chunk, size_t chunk_len,
                         uint8_t **out, size_t *out_len);

/**
 * Verify, parse, and consume a v2 envelope.  A used nonce or a message ID
 * previously marked complete is rejected; valid retransmission therefore
 * requires a newly wrapped fragment with a fresh nonce.
 */
bool cwist_seq_auth_unwrap(cwist_seq_auth_context_t *ctx,
                           const uint8_t *data, size_t len,
                           cwist_seq_auth_metadata_t *metadata,
                           cwist_seq_chunk_t *chunk);

/** Mark a successfully dispatched message ID as non-replayable. */
void cwist_seq_auth_mark_complete(cwist_seq_auth_context_t *ctx,
                                  const uint8_t message_id[CWIST_SEQ_AUTH_MESSAGE_ID_SIZE]);

#ifdef __cplusplus
}
#endif
#endif
