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
    size_t total_len;       ///< Reassembled length, known once final chunk arrives.
    size_t data_cap;        ///< Capacity for every possible full-size chunk.
    size_t max_data_len;    ///< Optional network-facing allocation limit.
    uint8_t *data;          ///< Reassembly buffer.
    bool *received;         ///< received[i] == true if chunk (i+1) arrived.
    uint16_t *payload_lens; ///< Accepted payload length for duplicate checks.
    uint16_t received_count;///< Number of distinct chunks received.
    bool contaminated;      ///< Conflicting duplicate observed; good bytes retained.
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
    if (out->seq == 0 || out->total == 0 || out->total > CWIST_SEQ_MAX_CHUNKS || out->seq > out->total) return false;
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
    if (total32 == 0 || total32 > UINT16_MAX || total32 > CWIST_SEQ_MAX_CHUNKS || len > CWIST_SEQ_MAX_REASSEMBLED_SIZE) return false;
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
    return cwist_seq_assembler_create_limited(0);
}

cwist_seq_assembler_t *cwist_seq_assembler_create_limited(size_t max_data_len) {
    cwist_seq_assembler_t *a = (cwist_seq_assembler_t *)cwist_alloc(sizeof(*a));
    if (!a) return NULL;
    memset(a, 0, sizeof(*a));
    a->max_data_len = max_data_len;
    return a;
}

void cwist_seq_assembler_destroy(cwist_seq_assembler_t *a) {
    if (!a) return;
    cwist_free(a->data);
    cwist_free(a->received);
    cwist_free(a->payload_lens);
    cwist_free(a);
}

void cwist_seq_assembler_reset(cwist_seq_assembler_t *a) {
    if (!a) return;
    cwist_free(a->data);
    cwist_free(a->received);
    cwist_free(a->payload_lens);
    memset(a, 0, sizeof(*a));
}

bool cwist_seq_assembler_feed(cwist_seq_assembler_t *a, const cwist_seq_chunk_t *chunk) {
    if (!a || !chunk) return false;

    /* Validate sequence pair. */
    if (chunk->seq == 0 || chunk->total == 0 || chunk->total > CWIST_SEQ_MAX_CHUNKS || chunk->seq > chunk->total) return false;
    if (chunk->payload_len == 0 || chunk->chunk_size == 0) return false;
    if (chunk->payload_len > chunk->chunk_size) return false;
    if (chunk->seq != chunk->total && chunk->payload_len != chunk->chunk_size) return false;
    if (!chunk->payload) return false;

    if (!a->have_state) {
        a->total = chunk->total;
        a->chunk_size = chunk->chunk_size;
        /* The first packet may be the short final packet.  Allocate for the
         * full layout and determine total_len only after seq == total is
         * actually received; deriving it from arrival order was the source of
         * false protocol errors on reordered HTTP/2/3-adjacent transports. */
        if ((size_t)chunk->total > SIZE_MAX / (size_t)chunk->chunk_size) return false;
        a->data_cap = (size_t)chunk->total * chunk->chunk_size;
        size_t ceiling = a->max_data_len ? a->max_data_len : CWIST_SEQ_MAX_REASSEMBLED_SIZE;
        if (a->data_cap > ceiling) {
            a->data_cap = 0;
            return false;
        }
        a->data = (uint8_t *)cwist_alloc(a->data_cap);
        a->received = (bool *)cwist_alloc_array(chunk->total, sizeof(bool));
        a->payload_lens = (uint16_t *)cwist_alloc_array(chunk->total, sizeof(uint16_t));
        if (!a->data || !a->received || !a->payload_lens) {
            cwist_free(a->data);
            cwist_free(a->received);
            cwist_free(a->payload_lens);
            memset(a, 0, sizeof(*a));
            return false;
        }
        a->have_state = true;
    } else {
        if (chunk->total != a->total || chunk->chunk_size != a->chunk_size) return false;
    }

    size_t index = (size_t)chunk->seq - 1;
    size_t offset = index * a->chunk_size;
    if (a->received[index]) {
        /* Safe duplicates are idempotent.  A different duplicate is data
         * corruption, not a retry, and must never overwrite good bytes. */
        bool identical = a->payload_lens[index] == chunk->payload_len &&
                         memcmp(a->data + offset, chunk->payload, chunk->payload_len) == 0;
        if (!identical) a->contaminated = true;
        return identical;
    }

    if (offset > a->data_cap || chunk->payload_len > a->data_cap - offset) return false;

    memcpy(a->data + offset, chunk->payload, chunk->payload_len);
    a->received[index] = true;
    a->payload_lens[index] = chunk->payload_len;
    if (chunk->seq == chunk->total) {
        a->total_len = offset + chunk->payload_len;
    }
    a->received_count++;
    return true;
}

bool cwist_seq_assembler_is_complete(const cwist_seq_assembler_t *a) {
    return a && a->have_state && !a->contaminated && a->received_count == a->total &&
           a->received[a->total - 1] && a->total_len > 0;
}

size_t cwist_seq_assembler_recovery_targets(const cwist_seq_assembler_t *a,
                                            uint16_t *out,
                                            size_t out_cap) {
    if (!a || !a->have_state || cwist_seq_assembler_is_complete(a)) return 0;

    size_t missing = 0;
    for (uint16_t i = 0; i < a->total; ++i) {
        if (!a->received[i]) {
            if (out && missing < out_cap) out[missing] = (uint16_t)(i + 1);
            missing++;
        }
    }
    return missing;
}

bool cwist_seq_assembler_get_data(cwist_seq_assembler_t *a,
                                  const uint8_t **out_data,
                                  size_t *out_len) {
    if (!cwist_seq_assembler_is_complete(a) || !out_data || !out_len) return false;
    *out_data = a->data;
    *out_len = a->total_len;
    return true;
}
