/**
 * @file protobuf.c
 * @brief Small Protobuf wire-format reader/writer helpers.
 */

#include <cwist/net/grpc/protobuf.h>
#include <cwist/core/mem/alloc.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void cwist_pb_writer_init(cwist_pb_writer *w) {
    if (!w) return;
    w->data = NULL;
    w->len = 0;
    w->cap = 0;
}

void cwist_pb_writer_free(cwist_pb_writer *w) {
    if (!w) return;
    cwist_free(w->data);
    w->data = NULL;
    w->len = 0;
    w->cap = 0;
}

int cwist_pb_writer_reserve(cwist_pb_writer *w, size_t extra) {
    if (!w) return -1;
    if (extra > SIZE_MAX - w->len) return -1;
    size_t need = w->len + extra;
    if (need <= w->cap) return 0;

    size_t cap = w->cap ? w->cap : 64;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    uint8_t *next = (uint8_t *)cwist_realloc(w->data, cap);
    if (!next) return -1;
    w->data = next;
    w->cap = cap;
    return 0;
}

int cwist_pb_write_varint(cwist_pb_writer *w, uint64_t value) {
    if (!w) return -1;
    if (cwist_pb_writer_reserve(w, 10) != 0) return -1;
    while (value >= 0x80) {
        w->data[w->len++] = (uint8_t)(value | 0x80);
        value >>= 7;
    }
    w->data[w->len++] = (uint8_t)value;
    return 0;
}

int cwist_pb_write_key(cwist_pb_writer *w, uint32_t field_number, cwist_pb_wire_type_t wire_type) {
    if (!w || field_number == 0) return -1;
    if (wire_type != CWIST_PB_VARINT && wire_type != CWIST_PB_64BIT &&
        wire_type != CWIST_PB_LEN && wire_type != CWIST_PB_32BIT) {
        return -1;
    }
    uint64_t key = ((uint64_t)field_number << 3) | (uint64_t)wire_type;
    return cwist_pb_write_varint(w, key);
}

int cwist_pb_write_uint64_field(cwist_pb_writer *w, uint32_t field_number, uint64_t value) {
    if (cwist_pb_write_key(w, field_number, CWIST_PB_VARINT) != 0) return -1;
    return cwist_pb_write_varint(w, value);
}

int cwist_pb_write_int64_field(cwist_pb_writer *w, uint32_t field_number, int64_t value) {
    return cwist_pb_write_uint64_field(w, field_number, (uint64_t)value);
}

int cwist_pb_write_bool_field(cwist_pb_writer *w, uint32_t field_number, int value) {
    return cwist_pb_write_uint64_field(w, field_number, value ? 1 : 0);
}

int cwist_pb_write_bytes_field(cwist_pb_writer *w, uint32_t field_number, const void *data, size_t len) {
    if (!w || (len > 0 && !data)) return -1;
    if (cwist_pb_write_key(w, field_number, CWIST_PB_LEN) != 0) return -1;
    if (cwist_pb_write_varint(w, (uint64_t)len) != 0) return -1;
    if (cwist_pb_writer_reserve(w, len) != 0) return -1;
    if (len > 0) memcpy(w->data + w->len, data, len);
    w->len += len;
    return 0;
}

int cwist_pb_write_string_field(cwist_pb_writer *w, uint32_t field_number, const char *value) {
    if (!value) return -1;
    return cwist_pb_write_bytes_field(w, field_number, value, strlen(value));
}

void cwist_pb_reader_init(cwist_pb_reader *r, const void *data, size_t len) {
    if (!r) return;
    r->data = (const uint8_t *)data;
    r->len = len;
    r->pos = 0;
}

int cwist_pb_read_varint(cwist_pb_reader *r, uint64_t *out) {
    if (!r || !out) return -1;
    uint64_t value = 0;
    unsigned shift = 0;
    while (r->pos < r->len && shift <= 63) {
        uint8_t byte = r->data[r->pos++];
        value |= ((uint64_t)(byte & 0x7f)) << shift;
        if ((byte & 0x80) == 0) {
            *out = value;
            return 0;
        }
        shift += 7;
    }
    return -1;
}

int cwist_pb_read_field(cwist_pb_reader *r, cwist_pb_field *field) {
    if (!r || !field || r->pos >= r->len) return 0;
    memset(field, 0, sizeof(*field));

    uint64_t key = 0;
    if (cwist_pb_read_varint(r, &key) != 0) return -1;
    field->number = (uint32_t)(key >> 3);
    field->wire_type = (cwist_pb_wire_type_t)(key & 0x07);
    if (field->number == 0) return -1;

    switch (field->wire_type) {
        case CWIST_PB_VARINT:
            if (cwist_pb_read_varint(r, &field->varint) != 0) return -1;
            return 1;
        case CWIST_PB_LEN: {
            uint64_t len = 0;
            if (cwist_pb_read_varint(r, &len) != 0) return -1;
            if (len > SIZE_MAX || (size_t)len > r->len - r->pos) return -1;
            field->bytes = r->data + r->pos;
            field->len = (size_t)len;
            r->pos += field->len;
            return 1;
        }
        case CWIST_PB_32BIT:
            if (r->len - r->pos < 4) return -1;
            field->bytes = r->data + r->pos;
            field->len = 4;
            r->pos += 4;
            return 1;
        case CWIST_PB_64BIT:
            if (r->len - r->pos < 8) return -1;
            field->bytes = r->data + r->pos;
            field->len = 8;
            r->pos += 8;
            return 1;
        default:
            return -1;
    }
}

int cwist_pb_skip_field(cwist_pb_reader *r, const cwist_pb_field *field) {
    (void)r;
    return field ? 0 : -1;
}

uint64_t cwist_pb_zigzag_encode(int64_t value) {
    return ((uint64_t)value << 1) ^ (uint64_t)(value >> 63);
}

int64_t cwist_pb_zigzag_decode(uint64_t value) {
    return (int64_t)((value >> 1) ^ (uint64_t)-((int64_t)value & 1));
}
