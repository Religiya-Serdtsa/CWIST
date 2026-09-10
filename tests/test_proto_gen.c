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

static void test_gadget_round_trip(void) {
    static uint64_t ids64[] = {7, 8, 0x1122334455667788ULL};
    static double weights[] = {0.5, -1.25, 3.75};
    static proto_gen_sample_Gadget_CountsEntry counts[] = {
        {"alpha", 10},
        {"beta", -20},
    };
    static proto_gen_sample_Gadget_LabelsEntry labels[] = {
        {7, "seven"},
        {9, "nine"},
    };
    proto_gen_sample_Gadget src = {
        .f32 = 0xDEADBEEFu,
        .f64 = 0x1122334455667788ULL,
        .sf32 = -123456,
        .sf64 = -9876543210LL,
        .ratio = 3.141592653589793,
        .score = 2.5f,
        .ids64 = ids64, .ids64_count = 3,
        .weights = weights, .weights_count = 3,
        .payload_case = proto_gen_sample_Gadget_payload_CASE_code,
        .payload = {.code = -99},
        .counts = counts, .counts_count = 2,
        .labels = labels, .labels_count = 2,
        .maybe = 42,
    };

    cwist_pb_writer w;
    cwist_pb_writer_init(&w);
    assert(proto_gen_sample_Gadget_encode(&w, &src) == 0);
    assert(w.len > 0);

    proto_gen_sample_Gadget dst;
    memset(&dst, 0, sizeof(dst));
    cwist_pb_reader r;
    cwist_pb_reader_init(&r, w.data, w.len);
    assert(proto_gen_sample_Gadget_decode(&r, &dst) == 0);
    assert(r.pos == r.len);

    assert(dst.f32 == src.f32);
    assert(dst.f64 == src.f64);
    assert(dst.sf32 == src.sf32);
    assert(dst.sf64 == src.sf64);
    assert(dst.ratio == src.ratio);
    assert(dst.score == src.score);
    assert(dst.ids64_count == 3);
    for (size_t i = 0; i < 3; i++) assert(dst.ids64[i] == src.ids64[i]);
    assert(dst.weights_count == 3);
    for (size_t i = 0; i < 3; i++) assert(dst.weights[i] == src.weights[i]);
    assert(dst.payload_case == proto_gen_sample_Gadget_payload_CASE_code);
    assert(dst.payload.code == -99);
    assert(dst.counts_count == 2);
    for (size_t i = 0; i < 2; i++) {
        assert(strcmp(dst.counts[i].key, src.counts[i].key) == 0);
        assert(dst.counts[i].value == src.counts[i].value);
    }
    assert(dst.labels_count == 2);
    for (size_t i = 0; i < 2; i++) {
        assert(dst.labels[i].key == src.labels[i].key);
        assert(strcmp(dst.labels[i].value, src.labels[i].value) == 0);
    }
    assert(dst.maybe == 42);

    /* decoded buffer re-encodes to the exact same bytes */
    cwist_pb_writer w2;
    cwist_pb_writer_init(&w2);
    assert(proto_gen_sample_Gadget_encode(&w2, &dst) == 0);
    assert(w2.len == w.len && memcmp(w.data, w2.data, w.len) == 0);

    proto_gen_sample_Gadget_free(&dst);
    cwist_pb_writer_free(&w);
    cwist_pb_writer_free(&w2);
}

static void test_gadget_oneof_arms(void) {
    /* string arm */
    proto_gen_sample_Gadget src;
    memset(&src, 0, sizeof(src));
    src.payload_case = proto_gen_sample_Gadget_payload_CASE_text;
    src.payload.text = "payload-text";

    cwist_pb_writer w;
    cwist_pb_writer_init(&w);
    assert(proto_gen_sample_Gadget_encode(&w, &src) == 0);

    proto_gen_sample_Gadget dst;
    memset(&dst, 0, sizeof(dst));
    cwist_pb_reader r;
    cwist_pb_reader_init(&r, w.data, w.len);
    assert(proto_gen_sample_Gadget_decode(&r, &dst) == 0);
    assert(dst.payload_case == proto_gen_sample_Gadget_payload_CASE_text);
    assert(strcmp(dst.payload.text, "payload-text") == 0);
    proto_gen_sample_Gadget_free(&dst);
    cwist_pb_writer_free(&w);

    /* message arm */
    static proto_gen_sample_Inner inner = {.note = "in-oneof", .delta = -5};
    memset(&src, 0, sizeof(src));
    src.payload_case = proto_gen_sample_Gadget_payload_CASE_inner;
    src.payload.inner = &inner;

    cwist_pb_writer_init(&w);
    assert(proto_gen_sample_Gadget_encode(&w, &src) == 0);
    memset(&dst, 0, sizeof(dst));
    cwist_pb_reader_init(&r, w.data, w.len);
    assert(proto_gen_sample_Gadget_decode(&r, &dst) == 0);
    assert(dst.payload_case == proto_gen_sample_Gadget_payload_CASE_inner);
    assert(dst.payload.inner != NULL);
    assert(strcmp(dst.payload.inner->note, "in-oneof") == 0);
    assert(dst.payload.inner->delta == -5);

    /* decoding a different arm must release the previous one */
    cwist_pb_writer_init(&w);
    assert(cwist_pb_write_string_field(&w, 9, "switched") == 0);
    cwist_pb_reader_init(&r, w.data, w.len);
    assert(proto_gen_sample_Gadget_decode(&r, &dst) == 0);
    assert(dst.payload_case == proto_gen_sample_Gadget_payload_CASE_text);
    assert(strcmp(dst.payload.text, "switched") == 0);

    proto_gen_sample_Gadget_free(&dst);
    cwist_pb_writer_free(&w);
}

static void test_gadget_unpacked_fixed(void) {
    /* non-packed repeated fixed64 (legal proto3) must decode like packed */
    cwist_pb_writer w;
    cwist_pb_writer_init(&w);
    assert(cwist_pb_write_fixed64_field(&w, 7, 100) == 0);
    assert(cwist_pb_write_fixed64_field(&w, 7, 200) == 0);
    assert(cwist_pb_write_fixed32_field(&w, 1, 0xABCDEF01u) == 0);

    proto_gen_sample_Gadget dst;
    memset(&dst, 0, sizeof(dst));
    cwist_pb_reader r;
    cwist_pb_reader_init(&r, w.data, w.len);
    assert(proto_gen_sample_Gadget_decode(&r, &dst) == 0);
    assert(dst.ids64_count == 2 && dst.ids64[0] == 100 && dst.ids64[1] == 200);
    assert(dst.f32 == 0xABCDEF01u);

    proto_gen_sample_Gadget_free(&dst);
    cwist_pb_writer_free(&w);
}

int main(void) {
    test_round_trip();
    test_unpacked_repeated_and_unknown_enum();
    test_unknown_fields_skipped();
    test_gadget_round_trip();
    test_gadget_oneof_arms();
    test_gadget_unpacked_fixed();
    assert(strcmp(proto_gen_sample_Greeter_SayHello_PATH,
                  "/proto_gen_sample.Greeter/SayHello") == 0);
    puts("test_proto_gen: all tests passed");
    return 0;
}
