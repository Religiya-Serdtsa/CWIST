#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cwist/net/http/http2_flow_control.h>

#define FRAME_SIZE 13U

static uint32_t read_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void assert_window_update(const uint8_t *frame,
                                 uint32_t stream_id,
                                 uint32_t increment) {
    assert(frame[0] == 0);
    assert(frame[1] == 0);
    assert(frame[2] == 4);
    assert(frame[3] == 0x08);
    assert(frame[4] == 0);
    assert(read_u32(frame + 5) == stream_id);
    assert(read_u32(frame + 9) == increment);
}

static void test_initialization_and_rtt(void) {
    cwist_http2_flow_control connection;
    cwist_http2_stream_flow_control stream;

    cwist_http2_flow_control_init(&connection, 1, 0);
    assert(connection.min_window == CWIST_HTTP2_INITIAL_CONNECTION_WINDOW);
    assert(connection.max_window == CWIST_HTTP2_INITIAL_CONNECTION_WINDOW);
    assert(connection.target_window == CWIST_HTTP2_INITIAL_CONNECTION_WINDOW);
    assert(connection.receive_window == CWIST_HTTP2_INITIAL_CONNECTION_WINDOW);
    assert(connection.send_window == CWIST_HTTP2_INITIAL_CONNECTION_WINDOW);
    assert(connection.pacing_tokens == CWIST_HTTP2_INITIAL_CONNECTION_WINDOW / 2U);

    cwist_http2_flow_control_init(&connection, 2U * 1024U * 1024U,
                                  8U * 1024U * 1024U);
    assert(connection.target_window == 2U * 1024U * 1024U);
    assert(connection.max_window == 8U * 1024U * 1024U);

    cwist_http2_flow_control_update_rtt(&connection, 0);
    assert(connection.srtt_us == 0);
    cwist_http2_flow_control_update_rtt(&connection, 800);
    assert(connection.srtt_us == 800);
    cwist_http2_flow_control_update_rtt(&connection, 1600);
    assert(connection.srtt_us == 900);

    cwist_http2_stream_flow_control_init(&stream, 3, 1, 0);
    assert(stream.stream_id == 3);
    assert(stream.min_window == CWIST_HTTP2_INITIAL_STREAM_WINDOW);
    assert(stream.target_window == CWIST_HTTP2_INITIAL_STREAM_WINDOW);
    assert(stream.receive_window == CWIST_HTTP2_INITIAL_STREAM_WINDOW);
    assert(stream.send_window == CWIST_HTTP2_INITIAL_STREAM_WINDOW);
}

static void test_receive_consume_and_connection_window_update(void) {
    cwist_http2_flow_control connection;
    uint8_t frame[FRAME_SIZE];
    size_t written = 99;
    uint32_t half;

    cwist_http2_flow_control_init(&connection, 0, 0);
    half = connection.target_window / 2U;

    assert(cwist_http2_flow_control_receive(&connection, half));
    assert(connection.receive_window == half);
    assert(!cwist_http2_flow_control_receive(&connection, half + 1U));
    assert(connection.receive_window == half);

    cwist_http2_flow_control_consume(&connection, half - 1U);
    assert(cwist_http2_flow_control_maybe_window_update(
               &connection, frame, sizeof(frame), &written, 100, false) == 0);
    assert(written == 0);

    cwist_http2_flow_control_consume(&connection, 1U);
    memset(frame, 0xff, sizeof(frame));
    assert(cwist_http2_flow_control_maybe_window_update(
               &connection, frame, sizeof(frame), &written, 100, false) == 1);
    assert(written == FRAME_SIZE);
    assert_window_update(frame, 0, half);
    assert(connection.receive_window == connection.target_window);
    assert(connection.pending_update == 0);
    assert(connection.last_window_update_us == 100);

    cwist_http2_flow_control_consume(&connection, 1U);
    assert(cwist_http2_flow_control_maybe_window_update(
               &connection, frame, FRAME_SIZE - 1U, &written, 101, true) == -1);
    assert(written == 0);
    assert(connection.pending_update == 1U);

    assert(cwist_http2_flow_control_maybe_window_update(
               &connection, frame, sizeof(frame), &written, 101, true) == 1);
    assert_window_update(frame, 0, 1U);

    assert(cwist_http2_flow_control_maybe_window_update(
               &connection, frame, sizeof(frame), &written, 102, true) == 0);
    assert(written == 0);
}

static void test_connection_auto_tuning(void) {
    cwist_http2_flow_control connection;
    uint8_t frame[FRAME_SIZE];
    size_t written;
    uint32_t initial;

    cwist_http2_flow_control_init(&connection,
                                  CWIST_HTTP2_INITIAL_CONNECTION_WINDOW,
                                  CWIST_HTTP2_INITIAL_CONNECTION_WINDOW * 4U);
    initial = connection.target_window;
    cwist_http2_flow_control_update_rtt(&connection, 100);

    assert(cwist_http2_flow_control_receive(&connection, initial / 2U));
    cwist_http2_flow_control_consume(&connection, initial / 2U);
    assert(cwist_http2_flow_control_maybe_window_update(
               &connection, frame, sizeof(frame), &written, 1000, false) == 1);
    assert(connection.target_window == initial);
    assert(connection.pacing_rate_bytes_per_sec ==
           (uint64_t)initial * 1000000U / 100U);

    assert(cwist_http2_flow_control_receive(&connection, initial / 2U));
    cwist_http2_flow_control_consume(&connection, initial / 2U);
    assert(cwist_http2_flow_control_maybe_window_update(
               &connection, frame, sizeof(frame), &written, 1050, false) == 1);
    assert(connection.target_window == initial * 2U);
    assert_window_update(frame, 0, initial * 3U / 2U);

    assert(cwist_http2_flow_control_receive(&connection, initial));
    cwist_http2_flow_control_consume(&connection, initial);
    assert(cwist_http2_flow_control_maybe_window_update(
               &connection, frame, sizeof(frame), &written, 1300, true) == 1);
    assert(connection.target_window == initial);
}

static void test_stream_window_update(void) {
    cwist_http2_flow_control connection;
    cwist_http2_stream_flow_control stream;
    cwist_http2_stream_flow_control invalid_stream;
    uint8_t frame[FRAME_SIZE];
    size_t written;
    uint32_t half;

    cwist_http2_flow_control_init(&connection, 0,
                                  CWIST_HTTP2_INITIAL_CONNECTION_WINDOW * 2U);
    cwist_http2_flow_control_update_rtt(&connection, 100);
    connection.last_window_update_us = 1000;

    cwist_http2_stream_flow_control_init(&stream, 7,
                                         CWIST_HTTP2_INITIAL_STREAM_WINDOW,
                                         CWIST_HTTP2_INITIAL_STREAM_WINDOW * 2U);
    half = stream.target_window / 2U;
    assert(cwist_http2_stream_flow_control_receive(&stream, half));
    assert(!cwist_http2_stream_flow_control_receive(&stream, half + 1U));
    cwist_http2_stream_flow_control_consume(&stream, half);
    assert(cwist_http2_stream_flow_control_maybe_window_update(
               &connection, &stream, frame, sizeof(frame), &written, 1050, false) == 1);
    assert(stream.target_window == CWIST_HTTP2_INITIAL_STREAM_WINDOW * 2U);
    assert_window_update(frame, 7, CWIST_HTTP2_INITIAL_STREAM_WINDOW * 3U / 2U);
    assert(stream.pending_update == 0);

    cwist_http2_stream_flow_control_init(&invalid_stream, 0, 0, 0);
    cwist_http2_stream_flow_control_consume(&invalid_stream, 1U);
    assert(cwist_http2_stream_flow_control_maybe_window_update(
               &connection, &invalid_stream, frame, sizeof(frame), &written, 1100, true) == 0);
    assert(written == 0);

    cwist_http2_stream_flow_control_init(&invalid_stream,
                                         CWIST_HTTP2_MAX_WINDOW + 1U, 0, 0);
    cwist_http2_stream_flow_control_consume(&invalid_stream, 1U);
    assert(cwist_http2_stream_flow_control_maybe_window_update(
               &connection, &invalid_stream, frame, sizeof(frame), &written, 1100, true) == 0);
}

static void test_send_windows_and_pacing(void) {
    cwist_http2_flow_control connection;
    cwist_http2_stream_flow_control stream;
    size_t allowance;

    cwist_http2_flow_control_init(&connection, 0, 0);
    cwist_http2_stream_flow_control_init(&stream, 1, 0, 0);

    connection.send_window = CWIST_HTTP2_MAX_WINDOW - 4U;
    cwist_http2_flow_control_add_send_window(&connection, 10U);
    assert(connection.send_window == CWIST_HTTP2_MAX_WINDOW);
    stream.send_window = CWIST_HTTP2_MAX_WINDOW - 4U;
    cwist_http2_stream_flow_control_add_send_window(&stream, 10U);
    assert(stream.send_window == CWIST_HTTP2_MAX_WINDOW);

    connection.send_window = 1000;
    stream.send_window = 700;
    connection.pacing_tokens = 500;
    connection.pacing_rate_bytes_per_sec = 0;
    connection.pacing_last_us = 1;
    allowance = cwist_http2_flow_control_pacing_allowance(&connection, &stream,
                                                           2000, 10);
    assert(allowance == 500);
    assert(cwist_http2_flow_control_reserve_send(&connection, &stream, 501, 10) == false);
    assert(cwist_http2_flow_control_reserve_send(&connection, &stream, 500, 10) == true);
    assert(connection.send_window == 500);
    assert(stream.send_window == 200);
    assert(connection.pacing_tokens == 0);

    connection.pacing_rate_bytes_per_sec = 1000000;
    connection.target_window = 65536;
    connection.pacing_last_us = 100;
    assert(cwist_http2_flow_control_pacing_allowance(&connection, &stream, 1000, 200) == 100);
    assert(connection.pacing_tokens == 100);
}

int main(void) {
    test_initialization_and_rtt();
    test_receive_consume_and_connection_window_update();
    test_connection_auto_tuning();
    test_stream_window_update();
    test_send_windows_and_pacing();
    puts("http2_flow_control tests passed");
    return 0;
}
