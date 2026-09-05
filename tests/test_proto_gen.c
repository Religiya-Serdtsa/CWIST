#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "test_proto_gen_sample.cwist.pb.h"

static proto_gen_sample_Outer build_sample(void) {
    static int64_t ids[] = {1, -2, 300};
    static const char *tags[] = {"alpha", "beta", "gamma"};
    static proto_gen_sample_Color shades[] = {
        proto_gen_sample_Color_COLOR_RED,
        proto_gen_sample_Color_COLOR_GREEN,
    };
    static proto_gen_sample_Inner items[] = {
        {.note = "first", .delta = -7},
        {.note = "second", .delta = 42},
    };
    static proto_gen_sample_Inner inner = {.note = "nested", .delta = -1000};
    proto_gen_sample_Outer m = {
        .name = "outer",
        .id = 9001,
        .active = 1,
        .ids = ids, .ids_count = 3,
        .tags = tags, .tags_count = 3,
        .inner = &inner,
        .items = items, .items_count = 2,
        .color = proto_gen_sample_Color_COLOR_GREEN,
        .shades = shades, .shades_count = 2,
    };
    return m;
}

static void assert_inner_equal(const proto_gen_sample_Inner *a, const proto_gen_sample_Inner *b) {
    assert(a && b);
    assert(strcmp(a->note, b->note) == 0);
    assert(a->delta == b->delta);
}

static void assert_outer_equal(const proto_gen_sample_Outer *a, const proto_gen_sample_Outer *b) {
    assert(strcmp(a->name, b->name) == 0);
    assert(a->id == b->id);
    assert(a->active == b->active);
    assert(a->ids_count == b->ids_count);
    for (size_t i = 0; i < a->ids_count; i++) assert(a->ids[i] == b->ids[i]);
    assert(a->tags_count == b->tags_count);
    for (size_t i = 0; i < a->tags_count; i++) assert(strcmp(a->tags[i], b->tags[i]) == 0);
    assert_inner_equal(a->inner, b->inner);
    assert(a->items_count == b->items_count);
    for (size_t i = 0; i < a->items_count; i++) assert_inner_equal(&a->items[i], &b->items[i]);
    assert(a->color == b->color);
    assert(a->shades_count == b->shades_count);
    for (size_t i = 0; i < a->shades_count; i++) assert(a->shades[i] == b->shades[i]);
}

static void test_round_trip(void) {
    proto_gen_sample_Outer src = build_sample();
    cwist_pb_writer w;
    cwist_pb_writer_init(&w);
    assert(proto_gen_sample_Outer_encode(&w, &src) == 0);
    assert(w.len > 0);

    proto_gen_sample_Outer dst;
    memset(&dst, 0, sizeof(dst));
    cwist_pb_reader r;
    cwist_pb_reader_init(&r, w.data, w.len);
    assert(proto_gen_sample_Outer_decode(&r, &dst) == 0);
    assert(r.pos == r.len);
    assert_outer_equal(&src, &dst);

    /* decoded buffer re-encodes to the exact same bytes */
    cwist_pb_writer w2;
    cwist_pb_writer_init(&w2);
    assert(proto_gen_sample_Outer_encode(&w2, &dst) == 0);
    assert(w2.len == w.len && memcmp(w.data, w2.data, w.len) == 0);

    proto_gen_sample_Outer_free(&dst);
    cwist_pb_writer_free(&w);
    cwist_pb_writer_free(&w2);
}

static void test_unpacked_repeated_and_unknown_enum(void) {
    /* non-packed repeated varints (legal proto3) and an unknown enum value
       must both survive decoding (proto3 open enum semantics) */
    cwist_pb_writer w;
    cwist_pb_writer_init(&w);
    assert(cwist_pb_write_uint64_field(&w, 4, 10) == 0); /* ids, unpacked */
    assert(cwist_pb_write_uint64_field(&w, 4, 20) == 0);
    assert(cwist_pb_write_uint64_field(&w, 8, 137) == 0); /* unknown Color */

    proto_gen_sample_Outer dst;
    memset(&dst, 0, sizeof(dst));
    cwist_pb_reader r;
    cwist_pb_reader_init(&r, w.data, w.len);
    assert(proto_gen_sample_Outer_decode(&r, &dst) == 0);
    assert(dst.ids_count == 2 && dst.ids[0] == 10 && dst.ids[1] == 20);
    assert((uint64_t)dst.color == 137);

    proto_gen_sample_Outer_free(&dst);
    cwist_pb_writer_free(&w);
}

static void test_unknown_fields_skipped(void) {
    proto_gen_sample_Outer src = build_sample();
    cwist_pb_writer w;
    cwist_pb_writer_init(&w);
    assert(proto_gen_sample_Outer_encode(&w, &src) == 0);
    assert(cwist_pb_write_string_field(&w, 99, "future field") == 0);

    proto_gen_sample_Outer dst;
    memset(&dst, 0, sizeof(dst));
    cwist_pb_reader r;
    cwist_pb_reader_init(&r, w.data, w.len);
    assert(proto_gen_sample_Outer_decode(&r, &dst) == 0);
    assert_outer_equal(&src, &dst);

    proto_gen_sample_Outer_free(&dst);
    cwist_pb_writer_free(&w);
}

int main(void) {
    test_round_trip();
    test_unpacked_repeated_and_unknown_enum();
    test_unknown_fields_skipped();
    assert(strcmp(proto_gen_sample_Greeter_SayHello_PATH,
                  "/proto_gen_sample.Greeter/SayHello") == 0);
    puts("test_proto_gen: all tests passed");
    return 0;
}
