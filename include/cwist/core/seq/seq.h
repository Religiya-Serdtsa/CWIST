/**
 * @file seq.h
 * @brief TCP-like sequenced message fragmentation and reassembly.
 *
 * Splits large payloads into small numbered chunks, transmits them, and
 * reassembles them even when they arrive out of order.  It also exposes the
 * exact missing sequence numbers so a transport can request retransmission
 * rather than treating a partially assembled body as complete.
 *
 * Chunk wire format (8 bytes, big-endian):
 *   seq          uint16_t  1-based sequence number
 *   total        uint16_t  total number of chunks in this message
 *   payload_len  uint16_t  bytes in this chunk
 *   chunk_size   uint16_t  payload bytes in a full chunk
 */

#ifndef __CWIST_CORE_SEQ_SEQ_H__
#define __CWIST_CORE_SEQ_SEQ_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Size of the sequence chunk header in bytes. */
#define CWIST_SEQ_HEADER_SIZE 8

/** Default payload size for each chunk when the caller does not care. */
#define CWIST_SEQ_DEFAULT_CHUNK_PAYLOAD 1024

/** Hard resource ceilings for network-facing reassembly, including callers
 * that use cwist_seq_assembler_create() without an explicit limit. */
#define CWIST_SEQ_MAX_CHUNKS 8192
#define CWIST_SEQ_MAX_REASSEMBLED_SIZE (64U * 1024U * 1024U)

/**
 * @brief Parsed view of one sequenced chunk.
 *
 * The payload pointer borrows the underlying buffer supplied to
 * cwist_seq_chunk_parse().
 */
typedef struct cwist_seq_chunk {
    uint16_t seq;           ///< 1-based sequence number.
    uint16_t total;         ///< Total chunks in the message.
    uint16_t payload_len;   ///< Payload length in this chunk.
    uint16_t chunk_size;    ///< Payload length of a full chunk.
    const uint8_t *payload; ///< Pointer to payload bytes.
} cwist_seq_chunk_t;

/**
 * @brief Split a message into sequenced chunks.
 *
 * Every chunk is an independent byte buffer prefixed with the 8-byte
 * sequence header.  The caller must free the returned buffers with
 * cwist_seq_message_free().
 */
typedef struct cwist_seq_message {
    uint8_t **chunks;    ///< Array of chunk buffers.
    size_t *chunk_lens;  ///< Length of each chunk buffer.
    size_t count;        ///< Number of chunks.
} cwist_seq_message_t;

/**
 * @brief Opaque stateful assembler that reorders chunks and detects duplicates.
 */
typedef struct cwist_seq_assembler cwist_seq_assembler_t;

/**
 * @brief Parse a sequence chunk from raw bytes.
 * @param data Raw bytes.
 * @param len Length of @p data.
 * @param out Receives parsed chunk fields.
 * @return true when @p data contains a valid chunk header and payload.
 */
bool cwist_seq_chunk_parse(const uint8_t *data, size_t len, cwist_seq_chunk_t *out);

/**
 * @brief Write an 8-byte sequence header.
 * @param out Output buffer of at least CWIST_SEQ_HEADER_SIZE bytes.
 * @param seq Sequence number.
 * @param total Total chunks.
 * @param payload_len Payload length in this chunk.
 * @param chunk_size Full chunk payload size.
 */
void cwist_seq_chunk_build_header(uint8_t out[CWIST_SEQ_HEADER_SIZE],
                                  uint16_t seq,
                                  uint16_t total,
                                  uint16_t payload_len,
                                  uint16_t chunk_size);

/**
 * @brief Split @p data into sequenced chunks.
 *
 * @param data Message bytes.
 * @param len Message length.
 * @param chunk_payload_size Maximum payload bytes per chunk (must be > 0).
 * @param out Receives the split message.  Caller must free with
 *            cwist_seq_message_free().
 * @return true on success, false on bad arguments or allocation failure.
 */
bool cwist_seq_split(const uint8_t *data,
                     size_t len,
                     uint16_t chunk_payload_size,
                     cwist_seq_message_t *out);

/**
 * @brief Free all buffers allocated by cwist_seq_split().
 */
void cwist_seq_message_free(cwist_seq_message_t *msg);

/**
 * @brief Create an empty assembler.
 */
cwist_seq_assembler_t *cwist_seq_assembler_create(void);

/**
 * @brief Create an assembler with a maximum possible reassembled size.
 *
 * A non-zero limit rejects a sequence layout whose full chunks would exceed
 * it before allocating its reassembly buffer.  Use this for network-facing
 * parsers.  A zero limit has the same behaviour as cwist_seq_assembler_create.
 */
cwist_seq_assembler_t *cwist_seq_assembler_create_limited(size_t max_data_len);

/**
 * @brief Destroy an assembler and its internal buffer.
 */
void cwist_seq_assembler_destroy(cwist_seq_assembler_t *a);

/**
 * @brief Feed one parsed chunk into the assembler.
 *
 * Duplicate chunks are silently ignored.  Chunks with missing or invalid
 * sequence pairs are rejected.
 *
 * @param a Assembler state.
 * @param chunk Parsed chunk.
 * @return true when the chunk was accepted (or was a duplicate).
 */
bool cwist_seq_assembler_feed(cwist_seq_assembler_t *a, const cwist_seq_chunk_t *chunk);

/**
 * @brief Return true when every expected chunk has been received.
 */
bool cwist_seq_assembler_is_complete(const cwist_seq_assembler_t *a);

/**
 * @brief Return the sequence numbers still required to complete a message.
 *
 * This is the ARQ recovery side of the sequenced protocol: callers can use
 * the returned 1-based values as retry targets.  The return value is the
 * total number of missing chunks, even when @p out is NULL or too small.
 * The result is zero for a complete assembler or one that has not yet seen a
 * valid chunk.
 *
 * @param a Assembler state.
 * @param out Optional output array for missing sequence numbers.
 * @param out_cap Number of entries available in @p out.
 * @return Number of missing chunks.
 */
size_t cwist_seq_assembler_recovery_targets(const cwist_seq_assembler_t *a,
                                            uint16_t *out,
                                            size_t out_cap);

/**
 * @brief Borrow the assembled message when complete.
 *
 * The returned pointer is valid until the assembler is destroyed or reset.
 *
 * @param a Assembler state.
 * @param out_data Receives pointer to assembled bytes.
 * @param out_len Receives assembled length.
 * @return true when the message is complete and outputs were written.
 */
bool cwist_seq_assembler_get_data(cwist_seq_assembler_t *a,
                                  const uint8_t **out_data,
                                  size_t *out_len);

/**
 * @brief Reset an assembler so it can receive a new message.
 */
void cwist_seq_assembler_reset(cwist_seq_assembler_t *a);

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_CORE_SEQ_SEQ_H__ */
