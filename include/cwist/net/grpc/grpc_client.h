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
