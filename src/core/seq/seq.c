#include <cwist/core/seq/seq.h>
#include <cwist/core/mem/alloc.h>

#include <string.h>

/**
 * @file seq.c
 * @brief Implementation of the CWIST sequenced-message protocol.
 */

struct cwist_seq_assembler {
    bool have_state;        ///< True after the first valid chunk was fed.
    uint16_t total;         ///< Expected number of chunks.
    uint16_t chunk_size;    ///< Payload size of a full chunk.
    size_t total_len;       ///< Reassembled message length.
    uint8_t *data;          ///< Reassembly buffer.
    bool *received;         ///< received[i] == true if chunk (i+1) arrived.
    uint16_t received_count;///< Number of distinct chunks received.
};

/* -------------------------------------------------------------------------- */
/* Header helpers                                                             */
/* -------------------------------------------------------------------------- */

static uint16_t seq_read_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void seq_write_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xff);
    p[1] = (uint8_t)(v & 0xff);
}

bool cwist_seq_chunk_parse(const uint8_t *data, size_t len, cwist_seq_chunk_t *out) {
    if (!data || !out || len < CWIST_SEQ_HEADER_SIZE) return false;

    out->seq = seq_read_u16(data + 0);
    out->total = seq_read_u16(data + 2);
    out->payload_len = seq_read_u16(data + 4);
    out->chunk_size = seq_read_u16(data + 6);
    out->payload = data + CWIST_SEQ_HEADER_SIZE;

    /* Reject missing or malformed sequence pairs. */
    if (out->seq == 0 || out->total == 0 || out->seq > out->total) return false;
    if (out->payload_len == 0 || out->chunk_size == 0) return false;
    if (out->payload_len > out->chunk_size) return false;
    if (out->seq != out->total && out->payload_len != out->chunk_size) return false;
    if (len - CWIST_SEQ_HEADER_SIZE < out->payload_len) return false;

    return true;
}

void cwist_seq_chunk_build_header(uint8_t out[CWIST_SEQ_HEADER_SIZE],
                                  uint16_t seq,
                                  uint16_t total,
                                  uint16_t payload_len,
                                  uint16_t chunk_size) {
    seq_write_u16(out + 0, seq);
    seq_write_u16(out + 2, total);
    seq_write_u16(out + 4, payload_len);
    seq_write_u16(out + 6, chunk_size);
}

/* -------------------------------------------------------------------------- */
/* Message splitter                                                           */
/* -------------------------------------------------------------------------- */

bool cwist_seq_split(const uint8_t *data,
                     size_t len,
                     uint16_t chunk_payload_size,
                     cwist_seq_message_t *out) {
    if (!data || len == 0 || chunk_payload_size == 0 || !out) return false;
    memset(out, 0, sizeof(*out));

    uint32_t total32 = (uint32_t)((len + chunk_payload_size - 1) / chunk_payload_size);
    if (total32 == 0 || total32 > UINT16_MAX) return false;
    uint16_t total = (uint16_t)total32;

    out->chunks = (uint8_t **)cwist_alloc_array(total, sizeof(uint8_t *));
    out->chunk_lens = (size_t *)cwist_alloc_array(total, sizeof(size_t));
    if (!out->chunks || !out->chunk_lens) {
        cwist_free(out->chunks);
        cwist_free(out->chunk_lens);
        memset(out, 0, sizeof(*out));
        return false;
    }

    for (uint16_t i = 0; i < total; i++) {
        size_t offset = (size_t)i * chunk_payload_size;
        size_t plen = len - offset;
        if (plen > chunk_payload_size) plen = chunk_payload_size;
        size_t chunk_len = CWIST_SEQ_HEADER_SIZE + plen;

        uint8_t *chunk = (uint8_t *)cwist_alloc(chunk_len);
        if (!chunk) {
            for (uint16_t j = 0; j < i; j++) cwist_free(out->chunks[j]);
            cwist_free(out->chunks);
            cwist_free(out->chunk_lens);
            memset(out, 0, sizeof(*out));
            return false;
        }

        cwist_seq_chunk_build_header(chunk, i + 1, total, (uint16_t)plen, chunk_payload_size);
        memcpy(chunk + CWIST_SEQ_HEADER_SIZE, data + offset, plen);
        out->chunks[i] = chunk;
        out->chunk_lens[i] = chunk_len;
    }

    out->count = total;
    return true;
}

void cwist_seq_message_free(cwist_seq_message_t *msg) {
    if (!msg) return;
    if (msg->chunks) {
        for (size_t i = 0; i < msg->count; i++) cwist_free(msg->chunks[i]);
        cwist_free(msg->chunks);
    }
    cwist_free(msg->chunk_lens);
    memset(msg, 0, sizeof(*msg));
}

/* -------------------------------------------------------------------------- */
/* Assembler                                                                  */
/* -------------------------------------------------------------------------- */

cwist_seq_assembler_t *cwist_seq_assembler_create(void) {
    cwist_seq_assembler_t *a = (cwist_seq_assembler_t *)cwist_alloc(sizeof(*a));
    if (!a) return NULL;
    memset(a, 0, sizeof(*a));
    return a;
}

void cwist_seq_assembler_destroy(cwist_seq_assembler_t *a) {
    if (!a) return;
    cwist_free(a->data);
    cwist_free(a->received);
    cwist_free(a);
}

void cwist_seq_assembler_reset(cwist_seq_assembler_t *a) {
    if (!a) return;
    cwist_free(a->data);
    cwist_free(a->received);
    memset(a, 0, sizeof(*a));
}

bool cwist_seq_assembler_feed(cwist_seq_assembler_t *a, const cwist_seq_chunk_t *chunk) {
    if (!a || !chunk) return false;

    /* Validate sequence pair. */
    if (chunk->seq == 0 || chunk->total == 0 || chunk->seq > chunk->total) return false;
    if (chunk->payload_len == 0 || chunk->chunk_size == 0) return false;
    if (chunk->payload_len > chunk->chunk_size) return false;
    if (chunk->seq != chunk->total && chunk->payload_len != chunk->chunk_size) return false;

    if (!a->have_state) {
        a->total = chunk->total;
        a->chunk_size = chunk->chunk_size;
        a->total_len = (size_t)(chunk->total - 1) * chunk->chunk_size + chunk->payload_len;
        a->data = (uint8_t *)cwist_alloc(a->total_len);
        a->received = (bool *)cwist_alloc_array(chunk->total, sizeof(bool));
        if (!a->data || !a->received) {
            cwist_free(a->data);
            cwist_free(a->received);
            memset(a, 0, sizeof(*a));
            return false;
        }
        a->have_state = true;
    } else {
        if (chunk->total != a->total || chunk->chunk_size != a->chunk_size) return false;
    }

    if (a->received[chunk->seq - 1]) return true; /* duplicate: discard */

    size_t offset = (size_t)(chunk->seq - 1) * a->chunk_size;
    if (offset + chunk->payload_len > a->total_len) return false;

    memcpy(a->data + offset, chunk->payload, chunk->payload_len);
    a->received[chunk->seq - 1] = true;
    a->received_count++;
    return true;
}

bool cwist_seq_assembler_is_complete(const cwist_seq_assembler_t *a) {
    return a && a->have_state && a->received_count == a->total;
}

bool cwist_seq_assembler_get_data(cwist_seq_assembler_t *a,
                                  const uint8_t **out_data,
                                  size_t *out_len) {
    if (!cwist_seq_assembler_is_complete(a) || !out_data || !out_len) return false;
    *out_data = a->data;
    *out_len = a->total_len;
    return true;
}
