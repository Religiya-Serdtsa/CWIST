/**
 * @file grpc_client.h
 * @brief HTTP/2-based gRPC client (h2c cleartext, or TLS with ALPN "h2").
 *
 * One active call per connection: cwist_grpc_call_start() fails while
 * another call on the same client is still open.  Payloads are opaque
 * serialized protobuf messages; the 5-byte gRPC framing is applied here.
 */

#ifndef __CWIST_GRPC_CLIENT_H__
#define __CWIST_GRPC_CLIENT_H__

#include <cwist/net/grpc/grpc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cwist_grpc_client cwist_grpc_client;
typedef struct cwist_grpc_call cwist_grpc_call;

typedef struct cwist_grpc_client_options {
    const char *authority;        /* :authority header; defaults to host:port */
    int use_tls;                  /* 0 = h2c cleartext, 1 = TLS (ALPN "h2") */
    int verify_peer;              /* TLS only: verify server certificate (default 1) */
    uint64_t connect_timeout_ms;  /* 0 = 10000 */
    const char *tls_server_name;  /* TLS only: SNI hostname; defaults to the
                                   * connect host (channels set this to the
                                   * original dns name, not the resolved IP) */
} cwist_grpc_client_options;

/**
 * Connect to host:port and perform the HTTP/2 handshake (client preface,
 * SETTINGS exchange).  Returns NULL on failure.
 */
cwist_grpc_client *cwist_grpc_client_connect(const char *host, uint16_t port,
                                             const cwist_grpc_client_options *options);

void cwist_grpc_client_close(cwist_grpc_client *client);

/**
 * Start a call to a full method path ("/package.Service/Method").
 * @p request is one serialized protobuf message (may be NULL/0); it is
 * framed and sent with END_STREAM, so the call covers unary and
 * server-streaming methods.  @p timeout_ms maps to the grpc-timeout header
 * and is also enforced locally: on expiry the stream is reset and the call
 * ends with CWIST_GRPC_DEADLINE_EXCEEDED.  Returns NULL on failure
 * (including when another call on this client is still active).
 */
cwist_grpc_call *cwist_grpc_call_start(cwist_grpc_client *client,
                                       const char *method,
                                       const void *request, size_t request_len,
                                       uint64_t timeout_ms);

/**
 * cwist_grpc_call_start() variant that attaches the standard
 * "grpc-previous-rpc-attempts" header (gRFC A6) when @p previous_attempts
 * is non-zero.  Used by the channel retry engine; plain callers want
 * cwist_grpc_call_start().
 */
cwist_grpc_call *cwist_grpc_call_start_ex(cwist_grpc_client *client,
                                          const char *method,
                                          const void *request, size_t request_len,
                                          uint64_t timeout_ms,
                                          uint32_t previous_attempts);

/**
 * Pump until the call's response headers arrive or the stream ends.
 * Returns 0 when Response-Headers were received (the call is committed,
 * gRFC A6), 1 when the stream ended first (status available via
 * cwist_grpc_call_finish), -1 on bad arguments.  Queued messages stay
 * buffered for cwist_grpc_call_recv().
 */
int cwist_grpc_call_await_headers(cwist_grpc_call *call);

/** Non-zero once Response-Headers arrived: the RPC is committed (gRFC A6). */
int cwist_grpc_call_committed(const cwist_grpc_call *call);

/**
 * Non-zero when the RPC provably never reached server application logic
 * (RST_STREAM REFUSED_STREAM, or GOAWAY with a last-stream-id below this
 * stream): gRFC A6 transparent-retry case 3.
 */
int cwist_grpc_call_refused(const cwist_grpc_call *call);

/**
 * gRFC A6 server pushback.  Returns 1 and fills @p out_ms when the server
 * sent "grpc-retry-pushback-ms"; a negative value asks the client not to
 * retry.  Returns 0 when the trailer was absent.
 */
int cwist_grpc_call_retry_pushback_ms(const cwist_grpc_call *call, int32_t *out_ms);

/** Non-zero when the connection can no longer start calls (dead or GOAWAY). */
int cwist_grpc_client_dead(cwist_grpc_client *client);

/** Last known grpc-status without pumping (CWIST_GRPC_OK until trailers). */
cwist_grpc_status_t cwist_grpc_call_status(const cwist_grpc_call *call);

/**
 * Receive the next response message.  Returns 1 and fills @p out when a
 * message arrived, 0 when the stream ended, -1 on transport/protocol error.
 * @p out->data is owned by the call and stays valid until the next recv or
 * cwist_grpc_call_destroy().
 */
int cwist_grpc_call_recv(cwist_grpc_call *call, cwist_grpc_message *out);

/**
 * Finish the call: waits for the server trailers when they have not arrived
 * yet, and returns the grpc-status code.  @p message receives the grpc-message
 * trailer value (valid until cwist_grpc_call_destroy), or NULL.
 */
cwist_grpc_status_t cwist_grpc_call_finish(cwist_grpc_call *call, const char **message);

/** Abort the call: sends RST_STREAM(CANCEL) unless the stream already ended. */
void cwist_grpc_call_cancel(cwist_grpc_call *call);

void cwist_grpc_call_destroy(cwist_grpc_call *call);

#ifdef __cplusplus
}
#endif

#endif
