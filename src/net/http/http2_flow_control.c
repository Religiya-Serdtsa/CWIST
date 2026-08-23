#include "cwist/net/http/http2_flow_control.h"

#include <limits.h>
#include <string.h>

static uint32_t
cwist_http2_window_clamp(uint64_t value, uint32_t minimum, uint32_t maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return (uint32_t)value;
}

static uint64_t
cwist_http2_abs_diff(uint64_t left, uint64_t right)
{
    return left >= right ? left - right : right - left;
}

static void
cwist_http2_flow_control_adjust(cwist_http2_flow_control *flow_control)
{
    uint64_t delay_us;
    uint64_t multiplier;

    if (!flow_control->rtt_initialized) {
        return;
    }

    delay_us = flow_control->srtt_us + (4U * flow_control->rttvar_us);
    if (flow_control->rttvar_us > flow_control->srtt_us / 2U) {
        flow_control->network_quality = CWIST_HTTP2_NETWORK_QUALITY_UNSTABLE;
    } else if (delay_us >= 200000U) {
        flow_control->network_quality = CWIST_HTTP2_NETWORK_QUALITY_HIGH_RTT;
    } else {
        flow_control->network_quality = CWIST_HTTP2_NETWORK_QUALITY_NORMAL;
    }

    multiplier = delay_us / 200000U;
    if (multiplier < 1U) {
        multiplier = 1U;
    }
    if (flow_control->network_quality != CWIST_HTTP2_NETWORK_QUALITY_NORMAL) {
        ++multiplier;
    }

    flow_control->target_window = cwist_http2_window_clamp(
        (uint64_t)flow_control->min_window * multiplier,
        flow_control->min_window,
        flow_control->max_window);
    flow_control->pacing_rate_bytes_per_sec =
        ((uint64_t)flow_control->target_window * 1000000U) /
        (flow_control->srtt_us ? flow_control->srtt_us : 1U);
}

static int
cwist_http2_write_window_update(uint8_t *buffer, size_t buffer_len,
                                size_t *written, uint32_t stream_id,
                                uint32_t increment)
{
    if (written != NULL) {
        *written = 0;
    }
    /* A WINDOW_UPDATE frame is 9 bytes of header + 4 bytes of payload;
     * accepting buffer_len < 13 used to write 4 bytes past the caller's
     * buffer. */
    if (buffer == NULL || written == NULL || buffer_len < 13U || increment == 0U) {
        return -1;
    }

    buffer[0] = 0;
    buffer[1] = 0;
    buffer[2] = 4;
    buffer[3] = 0x08;
    buffer[4] = 0;
    buffer[5] = (uint8_t)((stream_id >> 24) & 0x7fU);
    buffer[6] = (uint8_t)(stream_id >> 16);
    buffer[7] = (uint8_t)(stream_id >> 8);
    buffer[8] = (uint8_t)stream_id;
    buffer[9] = (uint8_t)(increment >> 24);
    buffer[10] = (uint8_t)(increment >> 16);
    buffer[11] = (uint8_t)(increment >> 8);
    buffer[12] = (uint8_t)increment;
    *written = 13U;
    return 0;
}

void
cwist_http2_flow_control_init(cwist_http2_flow_control *flow_control,
                              uint32_t initial_connection_window,
                              uint32_t max_connection_window)
{
    if (flow_control == NULL) {
        return;
    }
    /* Anything below the default minimum means "use the default". */
    if (initial_connection_window < CWIST_HTTP2_INITIAL_CONNECTION_WINDOW) {
        initial_connection_window = CWIST_HTTP2_INITIAL_CONNECTION_WINDOW;
    }
    if (max_connection_window < initial_connection_window) {
        max_connection_window = initial_connection_window;
    }
    if (max_connection_window > CWIST_HTTP2_MAX_WINDOW) {
        max_connection_window = CWIST_HTTP2_MAX_WINDOW;
    }

    memset(flow_control, 0, sizeof(*flow_control));
    flow_control->receive_window = initial_connection_window;
    flow_control->target_window = initial_connection_window;
    flow_control->min_window = initial_connection_window;
    flow_control->max_window = max_connection_window;
    flow_control->send_window = initial_connection_window;
    flow_control->network_quality = CWIST_HTTP2_NETWORK_QUALITY_NORMAL;
    /* Start with half a window of burst credit; the bucket refills at
     * pacing_rate once RTT samples calibrate it. */
    flow_control->pacing_tokens = initial_connection_window / 2U;
    flow_control->pacing_rate_bytes_per_sec = initial_connection_window * 10U;
}

void
cwist_http2_stream_flow_control_init(cwist_http2_stream_flow_control *flow_control,
                                     uint32_t stream_id,
                                     uint32_t initial_stream_window,
                                     uint32_t max_stream_window)
{
    if (flow_control == NULL) {
        return;
    }
    /* Anything below the default minimum means "use the default". */
    if (initial_stream_window < CWIST_HTTP2_INITIAL_STREAM_WINDOW) {
        initial_stream_window = CWIST_HTTP2_INITIAL_STREAM_WINDOW;
    }
    if (max_stream_window < initial_stream_window) {
        max_stream_window = initial_stream_window;
    }
    if (max_stream_window > CWIST_HTTP2_MAX_WINDOW) {
        max_stream_window = CWIST_HTTP2_MAX_WINDOW;
    }

    memset(flow_control, 0, sizeof(*flow_control));
    flow_control->stream_id = stream_id;
    flow_control->receive_window = initial_stream_window;
    flow_control->target_window = initial_stream_window;
    flow_control->min_window = initial_stream_window;
    flow_control->max_window = max_stream_window;
    flow_control->send_window = initial_stream_window;
}

void
cwist_http2_flow_control_update_rtt(cwist_http2_flow_control *flow_control,
                                    uint64_t rtt_sample_us)
{
    uint64_t difference;

    if (flow_control == NULL || rtt_sample_us == 0U) {
        return;
    }
    if (!flow_control->rtt_initialized) {
        flow_control->srtt_us = rtt_sample_us;
        flow_control->rttvar_us = rtt_sample_us / 2U;
        flow_control->rtt_initialized = true;
    } else {
        difference = cwist_http2_abs_diff(flow_control->srtt_us, rtt_sample_us);
        flow_control->rttvar_us = (3U * flow_control->rttvar_us + difference) / 4U;
        flow_control->srtt_us = (7U * flow_control->srtt_us + rtt_sample_us) / 8U;
    }
    cwist_http2_flow_control_adjust(flow_control);
}

bool
cwist_http2_flow_control_receive(cwist_http2_flow_control *flow_control, uint32_t bytes)
{
    if (flow_control == NULL || bytes > flow_control->receive_window) {
        return false;
    }
    flow_control->receive_window -= bytes;
    return true;
}

bool
cwist_http2_stream_flow_control_receive(cwist_http2_stream_flow_control *flow_control,
                                        uint32_t bytes)
{
    if (flow_control == NULL || bytes > flow_control->receive_window) {
        return false;
    }
    flow_control->receive_window -= bytes;
    return true;
}

void
cwist_http2_flow_control_consume(cwist_http2_flow_control *flow_control, uint32_t bytes)
{
    if (flow_control != NULL) {
        flow_control->pending_update += bytes > UINT32_MAX - flow_control->pending_update
                                            ? UINT32_MAX - flow_control->pending_update : bytes;
    }
}

void
cwist_http2_stream_flow_control_consume(cwist_http2_stream_flow_control *flow_control,
                                        uint32_t bytes)
{
    if (flow_control != NULL) {
        flow_control->pending_update += bytes > UINT32_MAX - flow_control->pending_update
                                            ? UINT32_MAX - flow_control->pending_update : bytes;
    }
}

/* Retune the receive target toward 2x the measured bandwidth-delay product:
 * bytes consumed since the previous update, projected over one SRTT. */
static uint32_t
cwist_http2_flow_control_retune_target(uint32_t pending_update,
                                       uint64_t srtt_us,
                                       uint64_t interval_us,
                                       uint32_t minimum,
                                       uint32_t maximum)
{
    if (interval_us == 0) {
        return minimum;
    }
    uint64_t bdp2 = (2ULL * (uint64_t)pending_update * srtt_us) / interval_us;
    return cwist_http2_window_clamp(bdp2, minimum, maximum);
}

/* Credit to hand back to the peer: top the window up to the target, but
 * never refund less than what the application has actually consumed. */
static uint32_t
cwist_http2_flow_control_increment(uint32_t pending_update,
                                   uint32_t receive_window,
                                   uint32_t target_window)
{
    uint32_t top_up = receive_window < target_window ? target_window - receive_window : 0U;
    return pending_update > top_up ? pending_update : top_up;
}

int
cwist_http2_flow_control_maybe_window_update(cwist_http2_flow_control *flow_control,
                                             uint8_t *buffer, size_t buffer_len,
                                             size_t *written, uint64_t now_us, bool force)
{
    uint32_t increment;

    if (written != NULL) {
        *written = 0;
    }
    if (flow_control == NULL || flow_control->pending_update == 0U ||
        (!force && flow_control->pending_update < flow_control->target_window / 2U)) {
        return 0;
    }

    if (flow_control->rtt_initialized && flow_control->last_window_update_us != 0U &&
        now_us > flow_control->last_window_update_us) {
        flow_control->target_window = cwist_http2_flow_control_retune_target(
            flow_control->pending_update, flow_control->srtt_us,
            now_us - flow_control->last_window_update_us,
            flow_control->min_window, flow_control->max_window);
    }

    increment = cwist_http2_flow_control_increment(flow_control->pending_update,
                                                   flow_control->receive_window,
                                                   flow_control->target_window);
    if (cwist_http2_write_window_update(buffer, buffer_len, written, 0U, increment) != 0) {
        return -1;
    }
    flow_control->receive_window =
        increment > CWIST_HTTP2_MAX_WINDOW - flow_control->receive_window
            ? CWIST_HTTP2_MAX_WINDOW
            : flow_control->receive_window + increment;
    flow_control->pending_update = 0;
    flow_control->last_window_update_us = now_us;
    return 1;
}

int
cwist_http2_stream_flow_control_maybe_window_update(cwist_http2_flow_control *connection_flow_control,
                                                    cwist_http2_stream_flow_control *stream_flow_control,
                                                    uint8_t *buffer, size_t buffer_len,
                                                    size_t *written, uint64_t now_us, bool force)
{
    uint32_t increment;

    if (written != NULL) {
        *written = 0;
    }
    if (connection_flow_control == NULL || stream_flow_control == NULL) {
        return -1;
    }
    /* Server-side only client-initiated (odd, non-zero) streams may receive
     * a stream-level WINDOW_UPDATE. */
    if (stream_flow_control->stream_id == 0U || (stream_flow_control->stream_id & 1U) == 0U) {
        return 0;
    }
    if (stream_flow_control->pending_update == 0U ||
        (!force && stream_flow_control->pending_update < stream_flow_control->target_window / 2U)) {
        return 0;
    }

    if (connection_flow_control->rtt_initialized &&
        connection_flow_control->last_window_update_us != 0U &&
        now_us > connection_flow_control->last_window_update_us) {
        stream_flow_control->target_window = cwist_http2_flow_control_retune_target(
            stream_flow_control->pending_update, connection_flow_control->srtt_us,
            now_us - connection_flow_control->last_window_update_us,
            stream_flow_control->min_window, stream_flow_control->max_window);
    }

    increment = cwist_http2_flow_control_increment(stream_flow_control->pending_update,
                                                   stream_flow_control->receive_window,
                                                   stream_flow_control->target_window);
    if (cwist_http2_write_window_update(buffer, buffer_len, written,
                                        stream_flow_control->stream_id, increment) != 0) {
        return -1;
    }
    stream_flow_control->receive_window =
        increment > CWIST_HTTP2_MAX_WINDOW - stream_flow_control->receive_window
            ? CWIST_HTTP2_MAX_WINDOW
            : stream_flow_control->receive_window + increment;
    stream_flow_control->pending_update = 0;
    stream_flow_control->last_window_update_us = now_us;
    return 1;
}

void
cwist_http2_flow_control_add_send_window(cwist_http2_flow_control *flow_control,
                                         uint32_t increment)
{
    if (flow_control != NULL) {
        /* Saturating add: credit is capped at the protocol maximum. */
        flow_control->send_window =
            increment > CWIST_HTTP2_MAX_WINDOW - flow_control->send_window
                ? CWIST_HTTP2_MAX_WINDOW
                : flow_control->send_window + increment;
    }
}

void
cwist_http2_stream_flow_control_add_send_window(cwist_http2_stream_flow_control *flow_control,
                                                uint32_t increment)
{
    if (flow_control != NULL) {
        flow_control->send_window =
            increment > CWIST_HTTP2_MAX_WINDOW - flow_control->send_window
                ? CWIST_HTTP2_MAX_WINDOW
                : flow_control->send_window + increment;
    }
}

size_t
cwist_http2_flow_control_pacing_allowance(cwist_http2_flow_control *connection_flow_control,
                                          const cwist_http2_stream_flow_control *stream_flow_control,
                                          size_t requested, uint64_t now_us)
{
    uint64_t elapsed;
    uint64_t added;
    size_t allowed;

    if (connection_flow_control == NULL || stream_flow_control == NULL) {
        return 0;
    }
    if (connection_flow_control->pacing_last_us == 0U) {
        connection_flow_control->pacing_last_us = now_us;
    } else if (now_us > connection_flow_control->pacing_last_us) {
        elapsed = now_us - connection_flow_control->pacing_last_us;
        added = (elapsed * connection_flow_control->pacing_rate_bytes_per_sec) / 1000000U;
        connection_flow_control->pacing_tokens += added;
        if (connection_flow_control->pacing_tokens > connection_flow_control->target_window) {
            connection_flow_control->pacing_tokens = connection_flow_control->target_window;
        }
        connection_flow_control->pacing_last_us = now_us;
    }

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

bool
cwist_http2_flow_control_reserve_send(cwist_http2_flow_control *connection_flow_control,
                                      cwist_http2_stream_flow_control *stream_flow_control,
                                      uint32_t bytes, uint64_t now_us)
{
    if (cwist_http2_flow_control_pacing_allowance(connection_flow_control, stream_flow_control,
                                                  bytes, now_us) < bytes) {
        return false;
    }
    connection_flow_control->send_window -= bytes;
    stream_flow_control->send_window -= bytes;
    connection_flow_control->pacing_tokens -= bytes;
    return true;
}
