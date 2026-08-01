#include <cwist/net/quic_flow_control.h>

#define QUIC_MAX_VARINT ((1ULL << 62) - 1)
#define QUIC_FRAME_MAX_DATA 0x10
#define QUIC_FRAME_MAX_STREAM_DATA 0x11

static uint64_t
clamp_window(uint64_t window)
{
    return window < QUIC_FLOW_CONTROL_MIN_INITIAL_WINDOW
        ? QUIC_FLOW_CONTROL_MIN_INITIAL_WINDOW : window;
}

static uint64_t
saturating_add(uint64_t value, uint64_t increment)
{
    return value > QUIC_MAX_VARINT - increment ? QUIC_MAX_VARINT : value + increment;
}

static size_t
varint_size(uint64_t value)
{
    if (value < (1ULL << 6))
        return 1;
    if (value < (1ULL << 14))
        return 2;
    if (value < (1ULL << 30))
        return 4;
    return 8;
}

static void
write_varint(uint8_t *buf, uint64_t value, size_t size)
{
    size_t i;

    for (i = size; i-- > 0; value >>= 8)
        buf[i] = (uint8_t)value;

    buf[0] |= (uint8_t)((size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3) << 6);
}

static int
should_update(uint64_t consumed, uint64_t maximum, uint64_t window)
{
    return consumed >= maximum || consumed + window / 2 >= maximum;
}

void
quic_flow_control_init(quic_conn_fc_t *fc, uint64_t initial_conn_window,
                       uint64_t initial_stream_window, uint64_t max_conn_window,
                       uint64_t max_stream_window)
{
    if (!fc)
        return;

    initial_conn_window = clamp_window(initial_conn_window);
    initial_stream_window = clamp_window(initial_stream_window);
    max_conn_window = clamp_window(max_conn_window);
    max_stream_window = clamp_window(max_stream_window);
    if (max_conn_window < initial_conn_window)
        max_conn_window = initial_conn_window;
    if (max_stream_window < initial_stream_window)
        max_stream_window = initial_stream_window;

    *fc = (quic_conn_fc_t){
        .quality = QUIC_NET_QUALITY_NORMAL,
        .max_data = initial_conn_window,
        .conn_window_size = initial_conn_window,
        .conn_min_window = initial_conn_window,
        .conn_max_window = max_conn_window,
        .base_stream_window = initial_stream_window,
        .stream_min_window = initial_stream_window,
        .stream_max_window = max_stream_window,
    };
}

void
quic_stream_fc_init(quic_stream_fc_t *sfc, uint64_t stream_id,
                    uint64_t initial_stream_window)
{
    if (!sfc)
        return;

    initial_stream_window = clamp_window(initial_stream_window);
    *sfc = (quic_stream_fc_t){
        .stream_id = stream_id,
        .max_stream_data = initial_stream_window,
        .stream_window_size = initial_stream_window,
    };
}

void
quic_flow_control_update_rtt(quic_conn_fc_t *fc, uint64_t rtt_sample_us)
{
    uint64_t difference;

    if (!fc || rtt_sample_us == 0)
        return;

    if (!fc->rtt_initialized) {
        fc->srtt_us = rtt_sample_us;
        fc->rttvar_us = rtt_sample_us / 2;
        fc->min_rtt_us = rtt_sample_us;
        fc->rtt_initialized = true;
    } else {
        difference = fc->srtt_us > rtt_sample_us
            ? fc->srtt_us - rtt_sample_us : rtt_sample_us - fc->srtt_us;
        fc->rttvar_us = (3 * fc->rttvar_us + difference) / 4;
        fc->srtt_us = (7 * fc->srtt_us + rtt_sample_us) / 8;
        if (rtt_sample_us < fc->min_rtt_us)
            fc->min_rtt_us = rtt_sample_us;
    }

    quic_flow_control_evaluate_quality(fc);
}

quic_net_quality_t
quic_flow_control_evaluate_quality(quic_conn_fc_t *fc)
{
    if (!fc || !fc->rtt_initialized)
        return QUIC_NET_QUALITY_NORMAL;

    if (fc->srtt_us <= 50000)
        fc->quality = QUIC_NET_QUALITY_EXCELLENT;
    else if (fc->srtt_us <= 150000)
        fc->quality = QUIC_NET_QUALITY_NORMAL;
    else if (fc->srtt_us <= 500000)
        fc->quality = QUIC_NET_QUALITY_HIGH_RTT;
    else
        fc->quality = QUIC_NET_QUALITY_POOR;

    return fc->quality;
}

void
quic_flow_control_adjust_windows(quic_conn_fc_t *fc, quic_stream_fc_t *sfc)
{
    if (!fc || fc->quality < QUIC_NET_QUALITY_HIGH_RTT)
        return;

    if (fc->conn_window_size < fc->conn_max_window) {
        fc->conn_window_size *= 2;
        if (fc->conn_window_size > fc->conn_max_window)
            fc->conn_window_size = fc->conn_max_window;
    }

    if (sfc && sfc->stream_window_size < fc->stream_max_window) {
        sfc->stream_window_size *= 2;
        if (sfc->stream_window_size > fc->stream_max_window)
            sfc->stream_window_size = fc->stream_max_window;
    }
}

void
quic_flow_control_consume_conn(quic_conn_fc_t *fc, uint64_t bytes)
{
    if (fc)
        fc->conn_consumed = saturating_add(fc->conn_consumed, bytes);
}

void
quic_flow_control_consume_stream(quic_conn_fc_t *fc, quic_stream_fc_t *sfc,
                                 uint64_t bytes)
{
    (void)fc;
    if (sfc)
        sfc->stream_consumed = saturating_add(sfc->stream_consumed, bytes);
}

int
quic_flow_control_maybe_send_max_data(quic_conn_fc_t *fc, uint8_t *buf,
                                      size_t buf_len, size_t *written, bool force)
{
    uint64_t maximum;
    size_t value_size;

    if (written)
        *written = 0;
    if (!fc || !buf || !written)
        return -1;
    if (!force && !should_update(fc->conn_consumed, fc->max_data, fc->conn_window_size))
        return 0;

    maximum = saturating_add(fc->conn_consumed, fc->conn_window_size);
    if (maximum < fc->max_data)
        maximum = fc->max_data;
    value_size = varint_size(maximum);
    if (buf_len < 1 + value_size)
        return -1;

    buf[0] = QUIC_FRAME_MAX_DATA;
    write_varint(buf + 1, maximum, value_size);
    fc->max_data = maximum;
    *written = 1 + value_size;
    return 1;
}

int
quic_flow_control_maybe_send_max_stream_data(quic_conn_fc_t *fc,
                                             quic_stream_fc_t *sfc,
                                             uint8_t *buf, size_t buf_len,
                                             size_t *written, bool force)
{
    uint64_t maximum;
    size_t id_size;
    size_t value_size;

    if (written)
        *written = 0;
    if (!fc || !sfc || !buf || !written)
        return -1;
    if (!force && !should_update(sfc->stream_consumed, sfc->max_stream_data,
                                 sfc->stream_window_size))
        return 0;

    maximum = saturating_add(sfc->stream_consumed, sfc->stream_window_size);
    if (maximum < sfc->max_stream_data)
        maximum = sfc->max_stream_data;
    id_size = varint_size(sfc->stream_id);
    value_size = varint_size(maximum);
    if (buf_len < 1 + id_size + value_size)
        return -1;

    buf[0] = QUIC_FRAME_MAX_STREAM_DATA;
    write_varint(buf + 1, sfc->stream_id, id_size);
    write_varint(buf + 1 + id_size, maximum, value_size);
    sfc->max_stream_data = maximum;
    *written = 1 + id_size + value_size;
    return 1;
}
