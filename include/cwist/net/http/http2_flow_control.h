#ifndef CWIST_NET_HTTP_HTTP2_FLOW_CONTROL_H
#define CWIST_NET_HTTP_HTTP2_FLOW_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CWIST_HTTP2_INITIAL_CONNECTION_WINDOW (1536U * 1024U)
#define CWIST_HTTP2_INITIAL_STREAM_WINDOW     (1024U * 1024U)
#define CWIST_HTTP2_MAX_WINDOW                 0x7fffffffU

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cwist_http2_flow_control {
    uint32_t receive_window;
    uint32_t target_window;
    uint32_t min_window;
    uint32_t max_window;
    uint32_t pending_update;
    uint32_t send_window;
    uint64_t srtt_us;
    uint64_t last_window_update_us;
    uint64_t pacing_tokens;
    uint64_t pacing_rate_bytes_per_sec;
    uint64_t pacing_last_us;
} cwist_http2_flow_control;

typedef struct cwist_http2_stream_flow_control {
    uint32_t stream_id;
    uint32_t receive_window;
    uint32_t target_window;
    uint32_t min_window;
    uint32_t max_window;
    uint32_t pending_update;
    uint32_t send_window;
    uint64_t last_window_update_us;
} cwist_http2_stream_flow_control;

void cwist_http2_flow_control_init(cwist_http2_flow_control *flow_control,
                                   uint32_t initial_connection_window,
                                   uint32_t max_connection_window);

void cwist_http2_stream_flow_control_init(cwist_http2_stream_flow_control *flow_control,
                                          uint32_t stream_id,
                                          uint32_t initial_stream_window,
                                          uint32_t max_stream_window);

void cwist_http2_flow_control_update_rtt(cwist_http2_flow_control *flow_control,
                                         uint64_t rtt_sample_us);

bool cwist_http2_flow_control_receive(cwist_http2_flow_control *flow_control,
                                      uint32_t bytes);

bool cwist_http2_stream_flow_control_receive(cwist_http2_stream_flow_control *flow_control,
                                             uint32_t bytes);

void cwist_http2_flow_control_consume(cwist_http2_flow_control *flow_control,
                                      uint32_t bytes);

void cwist_http2_stream_flow_control_consume(cwist_http2_stream_flow_control *flow_control,
                                             uint32_t bytes);

int cwist_http2_flow_control_maybe_window_update(cwist_http2_flow_control *flow_control,
                                                 uint8_t *buffer,
                                                 size_t buffer_len,
                                                 size_t *written,
                                                 uint64_t now_us,
                                                 bool force);

int cwist_http2_stream_flow_control_maybe_window_update(cwist_http2_flow_control *connection_flow_control,
                                                        cwist_http2_stream_flow_control *stream_flow_control,
                                                        uint8_t *buffer,
                                                        size_t buffer_len,
                                                        size_t *written,
                                                        uint64_t now_us,
                                                        bool force);

void cwist_http2_flow_control_add_send_window(cwist_http2_flow_control *flow_control,
                                              uint32_t increment);

void cwist_http2_stream_flow_control_add_send_window(cwist_http2_stream_flow_control *flow_control,
                                                     uint32_t increment);

size_t cwist_http2_flow_control_pacing_allowance(cwist_http2_flow_control *connection_flow_control,
                                                 const cwist_http2_stream_flow_control *stream_flow_control,
                                                 size_t requested,
                                                 uint64_t now_us);

bool cwist_http2_flow_control_reserve_send(cwist_http2_flow_control *connection_flow_control,
                                           cwist_http2_stream_flow_control *stream_flow_control,
                                           uint32_t bytes,
                                           uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif /* CWIST_NET_HTTP_HTTP2_FLOW_CONTROL_H */
