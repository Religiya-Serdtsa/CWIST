#ifndef CWIST_NET_QUIC_FLOW_CONTROL_H
#define CWIST_NET_QUIC_FLOW_CONTROL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define QUIC_FLOW_CONTROL_MIN_INITIAL_WINDOW (1024ULL * 1024ULL)

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    QUIC_NET_QUALITY_EXCELLENT = 0,
    QUIC_NET_QUALITY_NORMAL,
    QUIC_NET_QUALITY_HIGH_RTT,
    QUIC_NET_QUALITY_POOR
} quic_net_quality_t;

typedef struct quic_conn_fc {
    uint64_t srtt_us;
    uint64_t rttvar_us;
    uint64_t min_rtt_us;
    bool rtt_initialized;

    quic_net_quality_t quality;

    uint64_t max_data;
    uint64_t conn_consumed;
    uint64_t conn_window_size;
    uint64_t conn_min_window;
    uint64_t conn_max_window;

    uint64_t base_stream_window;
    uint64_t stream_min_window;
    uint64_t stream_max_window;
} quic_conn_fc_t;

typedef struct quic_stream_fc {
    uint64_t stream_id;
    uint64_t max_stream_data;
    uint64_t stream_consumed;
    uint64_t stream_window_size;
} quic_stream_fc_t;

void quic_flow_control_init(quic_conn_fc_t *fc,
                            uint64_t initial_conn_window,
                            uint64_t initial_stream_window,
                            uint64_t max_conn_window,
                            uint64_t max_stream_window);

void quic_stream_fc_init(quic_stream_fc_t *sfc, uint64_t stream_id, uint64_t initial_stream_window);

void quic_flow_control_update_rtt(quic_conn_fc_t *fc, uint64_t rtt_sample_us);

quic_net_quality_t quic_flow_control_evaluate_quality(quic_conn_fc_t *fc);

void quic_flow_control_adjust_windows(quic_conn_fc_t *fc, quic_stream_fc_t *sfc);

void quic_flow_control_consume_conn(quic_conn_fc_t *fc, uint64_t bytes);

void quic_flow_control_consume_stream(quic_conn_fc_t *fc, quic_stream_fc_t *sfc, uint64_t bytes);

int quic_flow_control_maybe_send_max_data(quic_conn_fc_t *fc,
                                           uint8_t *buf,
                                           size_t buf_len,
                                           size_t *written,
                                           bool force);

int quic_flow_control_maybe_send_max_stream_data(quic_conn_fc_t *fc,
                                                  quic_stream_fc_t *sfc,
                                                  uint8_t *buf,
                                                  size_t buf_len,
                                                  size_t *written,
                                                  bool force);

#ifdef __cplusplus
}
#endif

#endif /* CWIST_NET_QUIC_FLOW_CONTROL_H */
