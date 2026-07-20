/**
 * @file protobuf.h
 * @brief Small Protobuf wire-format reader/writer helpers.
 */

#ifndef __CWIST_PROTOBUF_H__
#define __CWIST_PROTOBUF_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cwist_pb_wire_type {
    CWIST_PB_VARINT = 0,
    CWIST_PB_64BIT = 1,
    CWIST_PB_LEN = 2,
    CWIST_PB_32BIT = 5,
} cwist_pb_wire_type_t;

typedef struct cwist_pb_writer {
    uint8_t *data;
    size_t len;
    size_t cap;
} cwist_pb_writer;

typedef struct cwist_pb_reader {
    const uint8_t *data;
    size_t len;
    size_t pos;
} cwist_pb_reader;

typedef struct cwist_pb_field {
    uint32_t number;
    cwist_pb_wire_type_t wire_type;
    uint64_t varint;
    const uint8_t *bytes;
    size_t len;
} cwist_pb_field;

void cwist_pb_writer_init(cwist_pb_writer *w);
void cwist_pb_writer_free(cwist_pb_writer *w);
int cwist_pb_writer_reserve(cwist_pb_writer *w, size_t extra);
int cwist_pb_write_varint(cwist_pb_writer *w, uint64_t value);
int cwist_pb_write_key(cwist_pb_writer *w, uint32_t field_number, cwist_pb_wire_type_t wire_type);
int cwist_pb_write_uint64_field(cwist_pb_writer *w, uint32_t field_number, uint64_t value);
int cwist_pb_write_int64_field(cwist_pb_writer *w, uint32_t field_number, int64_t value);
int cwist_pb_write_bool_field(cwist_pb_writer *w, uint32_t field_number, int value);
int cwist_pb_write_bytes_field(cwist_pb_writer *w, uint32_t field_number, const void *data, size_t len);
int cwist_pb_write_string_field(cwist_pb_writer *w, uint32_t field_number, const char *value);

void cwist_pb_reader_init(cwist_pb_reader *r, const void *data, size_t len);
int cwist_pb_read_varint(cwist_pb_reader *r, uint64_t *out);
int cwist_pb_read_field(cwist_pb_reader *r, cwist_pb_field *field);
int cwist_pb_skip_field(cwist_pb_reader *r, const cwist_pb_field *field);

uint64_t cwist_pb_zigzag_encode(int64_t value);
int64_t cwist_pb_zigzag_decode(uint64_t value);

#ifdef __cplusplus
}
#endif

#endif
