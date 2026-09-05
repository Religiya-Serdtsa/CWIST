/**
 * @file grpc.h
 * @brief gRPC over HTTP/2 helpers for CWIST.
 */

#ifndef __CWIST_GRPC_H__
#define __CWIST_GRPC_H__

#include <cwist/net/http/http.h>
#include <cwist/net/http/http2.h>
#include <cwist/net/grpc/protobuf.h>
#include <stddef.h>
#include <stdint.h>

struct cwist_app;

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cwist_grpc_status {
    CWIST_GRPC_OK = 0,
    CWIST_GRPC_CANCELLED = 1,
    CWIST_GRPC_UNKNOWN = 2,
    CWIST_GRPC_INVALID_ARGUMENT = 3,
    CWIST_GRPC_DEADLINE_EXCEEDED = 4,
    CWIST_GRPC_NOT_FOUND = 5,
    CWIST_GRPC_ALREADY_EXISTS = 6,
    CWIST_GRPC_PERMISSION_DENIED = 7,
    CWIST_GRPC_RESOURCE_EXHAUSTED = 8,
    CWIST_GRPC_FAILED_PRECONDITION = 9,
    CWIST_GRPC_ABORTED = 10,
    CWIST_GRPC_OUT_OF_RANGE = 11,
    CWIST_GRPC_UNIMPLEMENTED = 12,
    CWIST_GRPC_INTERNAL = 13,
    CWIST_GRPC_UNAVAILABLE = 14,
    CWIST_GRPC_DATA_LOSS = 15,
    CWIST_GRPC_UNAUTHENTICATED = 16,
} cwist_grpc_status_t;

typedef struct cwist_grpc_message {
    uint8_t compressed;
    const uint8_t *data;
    size_t len;
} cwist_grpc_message;

typedef struct cwist_grpc_stream {
    cwist_http_request *req;
    cwist_http_response *res;
    const cwist_grpc_message *messages;
    size_t message_count;
    cwist_grpc_status_t status;
    const char *status_message;
    int closed;
    int (*write_frame)(void *ctx, const uint8_t *frame, size_t frame_len, int end_stream);
    void *write_frame_ctx;
    /* Incremental streaming state (HTTP/2 transport path); NULL when the
     * stream was dispatched from a fully buffered request body. */
    void *session;
    int cancelled;
    uint64_t deadline_ms; /* monotonic deadline from grpc-timeout; 0 = none */
    size_t recv_pos;      /* buffered-path recv cursor */
} cwist_grpc_stream;

/** Incremental gRPC frame decoder for arbitrarily split HTTP/2 DATA payloads. */
typedef struct cwist_grpc_decoder {
    uint8_t header[5];
    size_t header_len;
    uint8_t *payload;
    size_t payload_len;
    size_t payload_used;
    uint8_t compressed;
    size_t max_message_size;
} cwist_grpc_decoder;

typedef int (*cwist_grpc_message_callback)(void *ctx,
                                           const cwist_grpc_message *message);

typedef void (*cwist_grpc_unary_handler_func)(cwist_http_request *req,
                                               cwist_http_response *res,
                                               const cwist_grpc_message *message,
                                               void *user_ctx);

typedef void (*cwist_grpc_stream_handler_func)(cwist_grpc_stream *stream,
                                                void *user_ctx);

int cwist_grpc_decode_message(const void *frame,
                              size_t frame_len,
                              cwist_grpc_message *out);

int cwist_grpc_decode_next_message(const void *frames,
                                   size_t frames_len,
                                   size_t *offset,
                                   cwist_grpc_message *out);

int cwist_grpc_encode_message(const void *payload,
                              size_t payload_len,
                              uint8_t compressed,
                              uint8_t **out,
                              size_t *out_len);

void cwist_grpc_set_response(cwist_http_response *res,
                             cwist_grpc_status_t status,
                             const char *message,
                             const void *payload,
                             size_t payload_len);

void cwist_grpc_set_error(cwist_http_response *res,
                          cwist_grpc_status_t status,
                          const char *message);

int cwist_grpc_stream_send(cwist_grpc_stream *stream,
                           const void *payload,
                           size_t payload_len);

void cwist_grpc_stream_close(cwist_grpc_stream *stream,
                             cwist_grpc_status_t status,
                             const char *message);

/** Attach a transport writer; subsequent sends are emitted immediately. */
void cwist_grpc_stream_set_writer(cwist_grpc_stream *stream,
                                  int (*write_frame)(void *, const uint8_t *, size_t, int),
                                  void *ctx);

/**
 * Blocking receive for streaming handlers on the HTTP/2 transport path.
 * Returns 1 and fills @p out when a message arrived, 0 on client
 * half-close (END_STREAM), and -1 when the call was cancelled (client
 * RST_STREAM, deadline expiry, or transport error).  On the buffered
 * (in-process) path this pops from the pre-decoded message array.
 * @p out->data stays valid until the next recv on the same stream.
 */
int cwist_grpc_stream_recv(cwist_grpc_stream *stream, cwist_grpc_message *out);

/** Non-zero when the call was cancelled or the deadline expired. */
int cwist_grpc_stream_cancelled(cwist_grpc_stream *stream);

/** Remaining deadline in milliseconds; UINT64_MAX when no grpc-timeout was set. */
uint64_t cwist_grpc_stream_deadline_remaining_ms(cwist_grpc_stream *stream);

/**
 * Normalized request metadata lookup.  Header names are matched
 * case-insensitively (HTTP/2 mandates lowercase); textual values are
 * returned as-is.  Returns NULL when the key is absent.
 */
const char *cwist_grpc_metadata_get(cwist_http_request *req, const char *key);

/**
 * Binary metadata ("*-bin" keys): base64-decodes the value into @p out.
 * Returns 0 on success, -1 when absent/undecodable, -2 when @p out_cap is
 * too small (required size reported in @p out_len).
 */
int cwist_grpc_metadata_get_binary(cwist_http_request *req, const char *key,
                                   uint8_t *out, size_t out_cap, size_t *out_len);

/** Parse a grpc-timeout header value ("100m", "2S", ...) into milliseconds.
 * Returns 0 on success, -1 on malformed input. */
int cwist_grpc_parse_timeout(const char *value, uint64_t *out_ms);

void cwist_grpc_decoder_init(cwist_grpc_decoder *decoder, size_t max_message_size);
void cwist_grpc_decoder_destroy(cwist_grpc_decoder *decoder);
/** Feed a partial HTTP/2 DATA payload.  Calls @p callback for every complete frame. */
int cwist_grpc_decoder_feed(cwist_grpc_decoder *decoder, const void *data, size_t len,
                            cwist_grpc_message_callback callback, void *ctx);

/** Register the standard grpc.health.v1.Health Check and Watch methods. */
int cwist_app_grpc_health(struct cwist_app *app);
int cwist_app_grpc_health_set_status(struct cwist_app *app, const char *service,
                                     int serving);

/** Register the grpc.reflection.v1alpha.ServerReflection service. */
int cwist_app_grpc_reflection(struct cwist_app *app);

int cwist_app_grpc_unary(struct cwist_app *app,
                         const char *service,
                         const char *method,
                         cwist_grpc_unary_handler_func handler,
                         void *user_ctx);

int cwist_app_grpc_stream(struct cwist_app *app,
                          const char *service,
                          const char *method,
                          cwist_grpc_stream_handler_func handler,
                          void *user_ctx);

/**
 * HTTP/2 stream hooks that route gRPC streaming calls through the
 * incremental transport path.  The hook context is the cwist_app pointer
 * already passed as user_ctx to cwist_http2_serve_connection_ex().
 */
const cwist_http2_stream_hooks *cwist_grpc_http2_hooks(void);

void cwist_grpc_routes_destroy(struct cwist_app *app);
int cwist_grpc_routes_clone(struct cwist_app *dst, const struct cwist_app *src);

#ifdef __cplusplus
}
#endif

#endif
