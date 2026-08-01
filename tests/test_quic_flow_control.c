#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cwist/net/quic_flow_control.h>

#define QMAX ((1ULL << 62) - 1)
#define MIN_WINDOW (1024ULL * 1024ULL)

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static size_t
encoded_varint_size(const uint8_t *buf)
{
    return (size_t)1 << (buf[0] >> 6);
}

static uint64_t
decode_varint(const uint8_t *buf, size_t *used)
{
    size_t i;
    size_t size = encoded_varint_size(buf);
    uint64_t value = buf[0] & 0x3f;

    for (i = 1; i < size; i++)
        value = (value << 8) | buf[i];

    if (used)
        *used = size;
    return value;
}

static int
test_initialization_and_clamping(void)
{
    quic_conn_fc_t fc;
    quic_stream_fc_t sfc;

    quic_flow_control_init(&fc, 0, 0, 0, 0);
    CHECK(fc.quality == QUIC_NET_QUALITY_NORMAL);
    CHECK(fc.max_data == MIN_WINDOW);
    CHECK(fc.conn_window_size == MIN_WINDOW);
    CHECK(fc.conn_min_window == MIN_WINDOW);
    CHECK(fc.conn_max_window == MIN_WINDOW);
    CHECK(fc.base_stream_window == MIN_WINDOW);
    CHECK(fc.stream_min_window == MIN_WINDOW);
    CHECK(fc.stream_max_window == MIN_WINDOW);
    CHECK(fc.conn_consumed == 0);
    CHECK(!fc.rtt_initialized);

    quic_flow_control_init(&fc, 2 * MIN_WINDOW, 3 * MIN_WINDOW,
                           MIN_WINDOW, MIN_WINDOW);
    CHECK(fc.conn_window_size == 2 * MIN_WINDOW);
    CHECK(fc.conn_max_window == 2 * MIN_WINDOW);
    CHECK(fc.base_stream_window == 3 * MIN_WINDOW);
    CHECK(fc.stream_min_window == 3 * MIN_WINDOW);
    CHECK(fc.stream_max_window == 3 * MIN_WINDOW);

    quic_stream_fc_init(&sfc, 123, 0);
    CHECK(sfc.stream_id == 123);
    CHECK(sfc.max_stream_data == MIN_WINDOW);
    CHECK(sfc.stream_window_size == MIN_WINDOW);
    CHECK(sfc.stream_consumed == 0);

    quic_flow_control_init(NULL, 1, 1, 1, 1);
    quic_stream_fc_init(NULL, 1, 1);
    return 0;
}

static int
test_rtt_quality(void)
{
    quic_conn_fc_t fc;

    quic_flow_control_init(&fc, MIN_WINDOW, MIN_WINDOW,
                           4 * MIN_WINDOW, 4 * MIN_WINDOW);
    CHECK(quic_flow_control_evaluate_quality(&fc) == QUIC_NET_QUALITY_NORMAL);

    quic_flow_control_update_rtt(&fc, 0);
    CHECK(!fc.rtt_initialized);

    quic_flow_control_update_rtt(&fc, 50000);
    CHECK(fc.rtt_initialized);
    CHECK(fc.srtt_us == 50000);
    CHECK(fc.rttvar_us == 25000);
    CHECK(fc.min_rtt_us == 50000);
    CHECK(fc.quality == QUIC_NET_QUALITY_EXCELLENT);

    quic_flow_control_update_rtt(&fc, 150000);
    CHECK(fc.srtt_us == 62500);
    CHECK(fc.rttvar_us == 43750);
    CHECK(fc.min_rtt_us == 50000);
    CHECK(fc.quality == QUIC_NET_QUALITY_NORMAL);

    fc.rtt_initialized = true;
    fc.srtt_us = 150001;
    CHECK(quic_flow_control_evaluate_quality(&fc) == QUIC_NET_QUALITY_HIGH_RTT);
    fc.srtt_us = 500001;
    CHECK(quic_flow_control_evaluate_quality(&fc) == QUIC_NET_QUALITY_POOR);
    fc.srtt_us = 150000;
    CHECK(quic_flow_control_evaluate_quality(&fc) == QUIC_NET_QUALITY_NORMAL);
    fc.srtt_us = 50000;
    CHECK(quic_flow_control_evaluate_quality(&fc) == QUIC_NET_QUALITY_EXCELLENT);
    CHECK(quic_flow_control_evaluate_quality(NULL) == QUIC_NET_QUALITY_NORMAL);

    return 0;
}

static int
test_window_adjustment(void)
{
    quic_conn_fc_t fc;
    quic_stream_fc_t sfc;

    quic_flow_control_init(&fc, MIN_WINDOW, MIN_WINDOW,
                           3 * MIN_WINDOW, 3 * MIN_WINDOW);
    quic_stream_fc_init(&sfc, 1, MIN_WINDOW);

    fc.quality = QUIC_NET_QUALITY_NORMAL;
    quic_flow_control_adjust_windows(&fc, &sfc);
    CHECK(fc.conn_window_size == MIN_WINDOW);
    CHECK(sfc.stream_window_size == MIN_WINDOW);

    fc.quality = QUIC_NET_QUALITY_HIGH_RTT;
    quic_flow_control_adjust_windows(&fc, &sfc);
    CHECK(fc.conn_window_size == 2 * MIN_WINDOW);
    CHECK(sfc.stream_window_size == 2 * MIN_WINDOW);

    quic_flow_control_adjust_windows(&fc, &sfc);
    CHECK(fc.conn_window_size == 3 * MIN_WINDOW);
    CHECK(sfc.stream_window_size == 3 * MIN_WINDOW);

    fc.conn_window_size = MIN_WINDOW;
    quic_flow_control_adjust_windows(&fc, NULL);
    CHECK(fc.conn_window_size == 2 * MIN_WINDOW);
    quic_flow_control_adjust_windows(NULL, &sfc);

    return 0;
}

static int
test_consumption_and_max_data_frames(void)
{
    quic_conn_fc_t fc;
    uint8_t buf[16];
    size_t written = 99;
    size_t used;
    uint64_t old_max;

    quic_flow_control_init(&fc, MIN_WINDOW, MIN_WINDOW,
                           4 * MIN_WINDOW, 4 * MIN_WINDOW);
    memset(buf, 0xa5, sizeof(buf));

    CHECK(quic_flow_control_maybe_send_max_data(&fc, buf, sizeof(buf),
                                                &written, false) == 0);
    CHECK(written == 0);
    CHECK(buf[0] == 0xa5);

    quic_flow_control_consume_conn(&fc, MIN_WINDOW / 2);
    CHECK(fc.conn_consumed == MIN_WINDOW / 2);
    CHECK(quic_flow_control_maybe_send_max_data(&fc, buf, sizeof(buf),
                                                &written, false) == 1);
    CHECK(buf[0] == 0x10);
    CHECK(written == 5);
    CHECK(decode_varint(buf + 1, &used) == 3 * MIN_WINDOW / 2);
    CHECK(used == 4);
    CHECK(fc.max_data == 3 * MIN_WINDOW / 2);

    CHECK(quic_flow_control_maybe_send_max_data(&fc, buf, sizeof(buf),
                                                &written, false) == 0);
    CHECK(written == 0);

    old_max = fc.max_data;
    CHECK(quic_flow_control_maybe_send_max_data(&fc, buf, 4, &written, true) == -1);
    CHECK(written == 0);
    CHECK(fc.max_data == old_max);

    CHECK(quic_flow_control_maybe_send_max_data(&fc, buf, sizeof(buf),
                                                &written, true) == 1);
    CHECK(decode_varint(buf + 1, NULL) == 3 * MIN_WINDOW / 2);

    fc.conn_consumed = QMAX - 2;
    quic_flow_control_consume_conn(&fc, 10);
    CHECK(fc.conn_consumed == QMAX);
    fc.max_data = MIN_WINDOW;
    CHECK(quic_flow_control_maybe_send_max_data(&fc, buf, sizeof(buf),
                                                &written, true) == 1);
    CHECK(written == 9);
    CHECK(decode_varint(buf + 1, &used) == QMAX);
    CHECK(used == 8);
    CHECK(fc.max_data == QMAX);

    written = 7;
    CHECK(quic_flow_control_maybe_send_max_data(NULL, buf, sizeof(buf),
                                                &written, false) == -1);
    CHECK(written == 0);
    CHECK(quic_flow_control_maybe_send_max_data(&fc, NULL, sizeof(buf),
                                                &written, false) == -1);
    CHECK(quic_flow_control_maybe_send_max_data(&fc, buf, sizeof(buf),
                                                NULL, false) == -1);
    quic_flow_control_consume_conn(NULL, 1);

    return 0;
}

static int
test_max_stream_data_frames(void)
{
    const uint64_t ids[] = { 63, 64, 16384, 1ULL << 30 };
    const size_t id_sizes[] = { 1, 2, 4, 8 };
    quic_conn_fc_t fc;
    quic_stream_fc_t sfc;
    uint8_t buf[24];
    size_t i;
    size_t written;
    size_t used;
    uint64_t old_max;

    quic_flow_control_init(&fc, MIN_WINDOW, MIN_WINDOW,
                           4 * MIN_WINDOW, 4 * MIN_WINDOW);

    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        quic_stream_fc_init(&sfc, ids[i], MIN_WINDOW);
        CHECK(quic_flow_control_maybe_send_max_stream_data(&fc, &sfc, buf,
                                                            sizeof(buf), &written,
                                                            true) == 1);
        CHECK(buf[0] == 0x11);
        CHECK(decode_varint(buf + 1, &used) == ids[i]);
        CHECK(used == id_sizes[i]);
        CHECK(decode_varint(buf + 1 + used, NULL) == MIN_WINDOW);
        CHECK(written == 1 + id_sizes[i] + 4);
    }

    quic_stream_fc_init(&sfc, 7, MIN_WINDOW);
    CHECK(quic_flow_control_maybe_send_max_stream_data(&fc, &sfc, buf,
                                                        sizeof(buf), &written,
                                                        false) == 0);
    CHECK(written == 0);

    quic_flow_control_consume_stream(NULL, &sfc, MIN_WINDOW / 2);
    CHECK(sfc.stream_consumed == MIN_WINDOW / 2);
    CHECK(quic_flow_control_maybe_send_max_stream_data(&fc, &sfc, buf,
                                                        sizeof(buf), &written,
                                                        false) == 1);
    CHECK(decode_varint(buf + 1, &used) == 7);
    CHECK(decode_varint(buf + 1 + used, NULL) == 3 * MIN_WINDOW / 2);
    CHECK(sfc.max_stream_data == 3 * MIN_WINDOW / 2);

    old_max = sfc.max_stream_data;
    CHECK(quic_flow_control_maybe_send_max_stream_data(&fc, &sfc, buf, 5,
                                                        &written, true) == -1);
    CHECK(written == 0);
    CHECK(sfc.max_stream_data == old_max);

    sfc.stream_consumed = QMAX - 1;
    quic_flow_control_consume_stream(&fc, &sfc, 2);
    CHECK(sfc.stream_consumed == QMAX);
    sfc.max_stream_data = MIN_WINDOW;
    sfc.stream_id = QMAX;
    CHECK(quic_flow_control_maybe_send_max_stream_data(&fc, &sfc, buf,
                                                        sizeof(buf), &written,
                                                        true) == 1);
    CHECK(written == 17);
    CHECK(decode_varint(buf + 1, &used) == QMAX);
    CHECK(used == 8);
    CHECK(decode_varint(buf + 1 + used, NULL) == QMAX);

    written = 1;
    CHECK(quic_flow_control_maybe_send_max_stream_data(NULL, &sfc, buf,
                                                        sizeof(buf), &written,
                                                        false) == -1);
    CHECK(written == 0);
    CHECK(quic_flow_control_maybe_send_max_stream_data(&fc, NULL, buf,
                                                        sizeof(buf), &written,
                                                        false) == -1);
    CHECK(quic_flow_control_maybe_send_max_stream_data(&fc, &sfc, NULL,
                                                        sizeof(buf), &written,
                                                        false) == -1);
    CHECK(quic_flow_control_maybe_send_max_stream_data(&fc, &sfc, buf,
                                                        sizeof(buf), NULL,
                                                        false) == -1);
    quic_flow_control_consume_stream(&fc, NULL, 1);

    return 0;
}

int
main(void)
{
    CHECK(test_initialization_and_clamping() == 0);
    CHECK(test_rtt_quality() == 0);
    CHECK(test_window_adjustment() == 0);
    CHECK(test_consumption_and_max_data_frames() == 0);
    CHECK(test_max_stream_data_frames() == 0);
    puts("quic_flow_control tests passed");
    return 0;
}
