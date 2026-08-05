#include <cwist/net/http/http2_flow_control.h>

#include <limits.h>
#include <string.h>

#define CWIST_HTTP2_WINDOW_UPDATE_FRAME_SIZE 13U
#define CWIST_HTTP2_MIN_PACING_BURST (16U * 1024U)

static uint32_t clamp_window(uint32_t value, uint32_t minimum, uint32_t maximum) {
    if (value < minimum) {
        return minimum;
    }
    return value > maximum ? maximum : value;
}

static uint32_t add_window(uint32_t window, uint32_t increment) {
    if (increment > CWIST_HTTP2_MAX_WINDOW - window) {
        return CWIST_HTTP2_MAX_WINDOW;
    }
    return window + increment;
}

static uint32_t update_increment(uint32_t receive_window,
                                 uint32_t target_window,
                                 uint32_t pending_update) {
    uint64_t increment = pending_update;

    if (target_window > receive_window + increment) {
        increment = target_window - receive_window;
    }
    return increment > CWIST_HTTP2_MAX_WINDOW ? CWIST_HTTP2_MAX_WINDOW : (uint32_t)increment;
}

static void encode_window_update(uint8_t *buffer, uint32_t stream_id, uint32_t increment) {
    buffer[0] = 0;
    buffer[1] = 0;
    buffer[2] = 4;
    buffer[3] = 0x08;
    buffer[4] = 0;
    buffer[5] = (uint8_t)(stream_id >> 24);
    buffer[6] = (uint8_t)(stream_id >> 16);
    buffer[7] = (uint8_t)(stream_id >> 8);
    buffer[8] = (uint8_t)stream_id;
    buffer[9] = (uint8_t)(increment >> 24);
    buffer[10] = (uint8_t)(increment >> 16);
    buffer[11] = (uint8_t)(increment >> 8);
    buffer[12] = (uint8_t)increment;
}

static void refresh_pacing(cwist_http2_flow_control *flow_control, uint64_t now_us) {
    uint64_t elapsed;
    uint64_t added;
    uint64_t burst;

    if (flow_control->pacing_last_us == 0) {
        flow_control->pacing_last_us = now_us;
        return;
    }
    elapsed = now_us - flow_control->pacing_last_us;
    flow_control->pacing_last_us = now_us;
    if (flow_control->pacing_rate_bytes_per_sec == 0 || elapsed == 0) {
        return;
    }
    added = elapsed > UINT64_MAX / flow_control->pacing_rate_bytes_per_sec
        ? UINT64_MAX
        : elapsed * flow_control->pacing_rate_bytes_per_sec / 1000000U;
    burst = flow_control->target_window / 2U;
    if (burst < CWIST_HTTP2_MIN_PACING_BURST) {
        burst = CWIST_HTTP2_MIN_PACING_BURST;
    }
    flow_control->pacing_tokens = flow_control->pacing_tokens + added > burst
        ? burst : flow_control->pacing_tokens + added;
}

static void adjust_connection_window(cwist_http2_flow_control *flow_control, uint64_t now_us) {
    uint64_t elapsed;

    if (flow_control->last_window_update_us != 0 && flow_control->srtt_us != 0) {
        elapsed = now_us - flow_control->last_window_update_us;
        if (elapsed <= flow_control->srtt_us && flow_control->target_window < flow_control->max_window) {
            flow_control->target_window = clamp_window(flow_control->target_window * 2U,
                                                        flow_control->min_window,
                                                        flow_control->max_window);
        } else if (elapsed > flow_control->srtt_us * 2U && flow_control->target_window > flow_control->min_window) {
            flow_control->target_window = clamp_window(flow_control->target_window / 2U,
                                                        flow_control->min_window,
                                                        flow_control->max_window);
        }
    }
    if (flow_control->srtt_us != 0) {
        flow_control->pacing_rate_bytes_per_sec =
            (uint64_t)flow_control->target_window * 1000000U / flow_control->srtt_us;
    }
}

void cwist_http2_flow_control_init(cwist_http2_flow_control *flow_control,
                                   uint32_t initial_connection_window,
                                   uint32_t max_connection_window) {
    uint32_t initial = initial_connection_window < CWIST_HTTP2_INITIAL_CONNECTION_WINDOW
        ? CWIST_HTTP2_INITIAL_CONNECTION_WINDOW : initial_connection_window;

    memset(flow_control, 0, sizeof(*flow_control));
    flow_control->min_window = CWIST_HTTP2_INITIAL_CONNECTION_WINDOW;
    flow_control->max_window = clamp_window(max_connection_window ? max_connection_window : initial,
                                            flow_control->min_window,
                                            CWIST_HTTP2_MAX_WINDOW);
    flow_control->target_window = clamp_window(initial, flow_control->min_window, flow_control->max_window);
    flow_control->receive_window = flow_control->target_window;
    flow_control->send_window = flow_control->target_window;
    flow_control->pacing_tokens = flow_control->target_window / 2U;
}

void cwist_http2_stream_flow_control_init(cwist_http2_stream_flow_control *flow_control,
                                          uint32_t stream_id,
                                          uint32_t initial_stream_window,
                                          uint32_t max_stream_window) {
    uint32_t initial = initial_stream_window < CWIST_HTTP2_INITIAL_STREAM_WINDOW
        ? CWIST_HTTP2_INITIAL_STREAM_WINDOW : initial_stream_window;

    memset(flow_control, 0, sizeof(*flow_control));
    flow_control->stream_id = stream_id;
    flow_control->min_window = CWIST_HTTP2_INITIAL_STREAM_WINDOW;
    flow_control->max_window = clamp_window(max_stream_window ? max_stream_window : initial,
                                            flow_control->min_window,
                                            CWIST_HTTP2_MAX_WINDOW);
    flow_control->target_window = clamp_window(initial, flow_control->min_window, flow_control->max_window);
    flow_control->receive_window = flow_control->target_window;
    flow_control->send_window = flow_control->target_window;
}

void cwist_http2_flow_control_update_rtt(cwist_http2_flow_control *flow_control,
                                         uint64_t rtt_sample_us) {
    if (rtt_sample_us == 0) {
        return;
    }
    flow_control->srtt_us = flow_control->srtt_us == 0
        ? rtt_sample_us : (flow_control->srtt_us * 7U + rtt_sample_us) / 8U;
}

bool cwist_http2_flow_control_receive(cwist_http2_flow_control *flow_control, uint32_t bytes) {
    if (bytes > flow_control->receive_window) {
        return false;
    }
    flow_control->receive_window -= bytes;
    return true;
}

bool cwist_http2_stream_flow_control_receive(cwist_http2_stream_flow_control *flow_control,
                                             uint32_t bytes) {
    if (bytes > flow_control->receive_window) {
        return false;
    }
    flow_control->receive_window -= bytes;
    return true;
}

void cwist_http2_flow_control_consume(cwist_http2_flow_control *flow_control, uint32_t bytes) {
    flow_control->pending_update = add_window(flow_control->pending_update, bytes);
}

void cwist_http2_stream_flow_control_consume(cwist_http2_stream_flow_control *flow_control,
                                             uint32_t bytes) {
    flow_control->pending_update = add_window(flow_control->pending_update, bytes);
}

int cwist_http2_flow_control_maybe_window_update(cwist_http2_flow_control *flow_control,
                                                 uint8_t *buffer,
                                                 size_t buffer_len,
                                                 size_t *written,
                                                 uint64_t now_us,
                                                 bool force) {
    uint32_t increment;

    *written = 0;
    if (!force && flow_control->pending_update < flow_control->target_window / 2U) {
        return 0;
    }
    if (flow_control->pending_update == 0 || buffer_len < CWIST_HTTP2_WINDOW_UPDATE_FRAME_SIZE) {
        return flow_control->pending_update == 0 ? 0 : -1;
    }
    adjust_connection_window(flow_control, now_us);
    increment = update_increment(flow_control->receive_window, flow_control->target_window,
                                 flow_control->pending_update);
    encode_window_update(buffer, 0, increment);
    flow_control->receive_window = add_window(flow_control->receive_window, increment);
    flow_control->pending_update = 0;
    flow_control->last_window_update_us = now_us;
    *written = CWIST_HTTP2_WINDOW_UPDATE_FRAME_SIZE;
    return 1;
}

int cwist_http2_stream_flow_control_maybe_window_update(cwist_http2_flow_control *connection_flow_control,
                                                        cwist_http2_stream_flow_control *stream_flow_control,
                                                        uint8_t *buffer,
                                                        size_t buffer_len,
                                                        size_t *written,
                                                        uint64_t now_us,
                                                        bool force) {
    uint32_t increment;

    *written = 0;
    if (stream_flow_control->stream_id == 0 || stream_flow_control->stream_id > CWIST_HTTP2_MAX_WINDOW ||
        (!force && stream_flow_control->pending_update < stream_flow_control->target_window / 2U)) {
        return 0;
    }
    if (stream_flow_control->pending_update == 0 || buffer_len < CWIST_HTTP2_WINDOW_UPDATE_FRAME_SIZE) {
        return stream_flow_control->pending_update == 0 ? 0 : -1;
    }
    if (connection_flow_control->last_window_update_us != 0 && connection_flow_control->srtt_us != 0 &&
        now_us - connection_flow_control->last_window_update_us <= connection_flow_control->srtt_us &&
        stream_flow_control->target_window < stream_flow_control->max_window) {
        stream_flow_control->target_window = clamp_window(stream_flow_control->target_window * 2U,
                                                           stream_flow_control->min_window,
                                                           stream_flow_control->max_window);
    }
    increment = update_increment(stream_flow_control->receive_window, stream_flow_control->target_window,
                                 stream_flow_control->pending_update);
    encode_window_update(buffer, stream_flow_control->stream_id, increment);
    stream_flow_control->receive_window = add_window(stream_flow_control->receive_window, increment);
    stream_flow_control->pending_update = 0;
    stream_flow_control->last_window_update_us = now_us;
    *written = CWIST_HTTP2_WINDOW_UPDATE_FRAME_SIZE;
    return 1;
}

void cwist_http2_flow_control_add_send_window(cwist_http2_flow_control *flow_control,
                                              uint32_t increment) {
    flow_control->send_window = add_window(flow_control->send_window, increment);
}

void cwist_http2_stream_flow_control_add_send_window(cwist_http2_stream_flow_control *flow_control,
                                                     uint32_t increment) {
    flow_control->send_window = add_window(flow_control->send_window, increment);
}

size_t cwist_http2_flow_control_pacing_allowance(cwist_http2_flow_control *connection_flow_control,
                                                 const cwist_http2_stream_flow_control *stream_flow_control,
                                                 size_t requested,
                                                 uint64_t now_us) {
    size_t allowed;

    refresh_pacing(connection_flow_control, now_us);
    allowed = requested;
    if (allowed > connection_flow_control->send_window) {
        allowed = connection_flow_control->send_window;
    }
    if (allowed > stream_flow_control->send_window) {
        allowed = stream_flow_control->send_window;
    }
    if (allowed > connection_flow_control->pacing_tokens) {
        allowed = (size_t)connection_flow_control->pacing_tokens;
    }
    return allowed;
}

bool cwist_http2_flow_control_reserve_send(cwist_http2_flow_control *connection_flow_control,
                                           cwist_http2_stream_flow_control *stream_flow_control,
                                           uint32_t bytes,
                                           uint64_t now_us) {
    if (cwist_http2_flow_control_pacing_allowance(connection_flow_control, stream_flow_control, bytes, now_us) < bytes) {
        return false;
    }
    connection_flow_control->send_window -= bytes;
    stream_flow_control->send_window -= bytes;
    connection_flow_control->pacing_tokens -= bytes;
    return true;
}
