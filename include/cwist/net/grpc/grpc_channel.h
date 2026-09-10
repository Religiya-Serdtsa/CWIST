/**
 * @file grpc_channel.h
 * @brief gRPC client channel: name resolution, client-side load balancing,
 *        and gRFC A6 retry policy on top of cwist_grpc_client.
 *
 * A channel resolves one target (doc/naming.md) into a list of backend
 * addresses, keeps one lazily-connected subchannel per address, picks a
 * subchannel per call with the configured LB policy (pick_first default,
 * or round_robin — doc/load-balancing.md), and retries failed RPCs between
 * the channel and the LB pick, so every attempt can land on a different
 * subchannel (gRFC A6).
 *
 * Subchannel reconnection follows doc/connection-backoff.md: 1s initial
 * backoff, x1.6 multiplier, 120s cap, +/-0.2 jitter, reset once the HTTP/2
 * SETTINGS handshake completes.
 */

#ifndef __CWIST_GRPC_CHANNEL_H__
#define __CWIST_GRPC_CHANNEL_H__

#include <cwist/net/grpc/grpc_client.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cwist_grpc_channel cwist_grpc_channel;
typedef struct cwist_grpc_channel_call cwist_grpc_channel_call;

typedef enum cwist_grpc_lb_policy {
    CWIST_GRPC_LB_PICK_FIRST = 0,   /* default (doc/load-balancing.md) */
    CWIST_GRPC_LB_ROUND_ROBIN = 1,
} cwist_grpc_lb_policy;

typedef enum cwist_grpc_channel_state {
    CWIST_GRPC_CHANNEL_IDLE = 0,
    CWIST_GRPC_CHANNEL_CONNECTING,
    CWIST_GRPC_CHANNEL_READY,
    CWIST_GRPC_CHANNEL_TRANSIENT_FAILURE,
    CWIST_GRPC_CHANNEL_SHUTDOWN,
} cwist_grpc_channel_state;

/** Bit for @ref cwist_grpc_retry_policy::retryable_status_mask (code 0..16). */
#define CWIST_GRPC_STATUS_BIT(code) (1u << (code))

/**
 * gRFC A6 retryPolicy.  Retries are disabled unless a policy is configured
 * (channel option or JSON service config).
 */
typedef struct cwist_grpc_retry_policy {
    uint32_t max_attempts;         /* RPC attempts incl. the original; >= 2,
                                    * clamped to the client-side maximum of 5 */
    uint64_t initial_backoff_ms;   /* > 0; first retry after
                                    * initial_backoff * random(0.8, 1.2) */
    uint64_t max_backoff_ms;       /* > 0 */
    double backoff_multiplier;     /* > 0; nth retry waits
                                    * min(initial*mult^(n-1), max) * random(0.8,1.2) */
    uint32_t retryable_status_mask; /* CWIST_GRPC_STATUS_BIT() per retryable code */
} cwist_grpc_retry_policy;

/** gRFC A6 retryThrottling (per channel, i.e. per server name). */
typedef struct cwist_grpc_retry_throttle {
    double max_tokens;   /* (0, 1000]; token_count starts here */
    double token_ratio;  /* > 0; added to token_count per successful RPC */
} cwist_grpc_retry_throttle;

typedef struct cwist_grpc_channel_options {
    const char *authority;        /* :authority header; defaults to the target host:port */
    int use_tls;                  /* 0 = h2c cleartext, 1 = TLS (ALPN "h2") */
    int verify_peer;              /* TLS only: verify server certificate (default 1) */
    uint64_t connect_timeout_ms;  /* 0 = 10000 */
    cwist_grpc_lb_policy lb_policy;                      /* default pick_first */
    const cwist_grpc_retry_policy *retry_policy;         /* NULL = retries disabled (A6 default) */
    const cwist_grpc_retry_throttle *retry_throttle;     /* NULL = no throttling */
    uint64_t per_rpc_buffer_limit; /* 0 = 256 KiB; requests larger than this
                                    * are sent but never retried (A6 buffering) */
    int wait_for_ready;           /* 0 = fail fast UNAVAILABLE while no
                                   * subchannel is READY; 1 = wait until the
                                   * call deadline */
} cwist_grpc_channel_options;

/**
 * Resolve @p target and create a channel.  Target syntax (doc/naming.md):
 *   dns:[//authority/]host[:port]  (default scheme; authority ignored;
 *                                   all resolved addresses are used;
 *                                   default port 443)
 *   ipv4:address[:port][,address[:port],...]
 *   ipv6:address[:port][,address[:port],...]  ([] literals when a port is given)
 *   host[:port]                   (no scheme = dns)
 * Returns NULL when the target is invalid, the scheme is unsupported
 * (unix/vsock/... are not implemented), or no address resolves.
 */
cwist_grpc_channel *cwist_grpc_channel_connect(const char *target,
                                               const cwist_grpc_channel_options *options);

void cwist_grpc_channel_close(cwist_grpc_channel *channel);

/** Aggregated channel connectivity state (doc/load-balancing.md rules). */
cwist_grpc_channel_state cwist_grpc_channel_get_state(cwist_grpc_channel *channel);

/**
 * Apply a JSON service config (doc/service_config.md subset):
 * "loadBalancingConfig" ([{"round_robin":{}}, ...] — first supported policy
 * wins), the deprecated "loadBalancingPolicy" string, "methodConfig" entries
 * with "name" (service/method), "retryPolicy", "waitForReady" and "timeout",
 * and "retryThrottling".  Validation follows gRFC A6 (maxAttempts integer
 * greater than 1 clamped to 5, proto3-JSON Duration strings, non-empty
 * retryableStatusCodes in integer or case-insensitive string form,
 * maxTokens in (0,1000], tokenRatio > 0).  Returns 0 on success, -1 on a
 * validation error (channel configuration is left unchanged).
 */
int cwist_grpc_channel_apply_service_config_json(cwist_grpc_channel *channel,
                                                 const char *json);

/**
 * Start a call through the channel: picks a subchannel per attempt and
 * applies the retry policy.  The returned call is either committed
 * (Response-Headers arrived) or final (all attempts exhausted or a
 * non-retryable failure); recv/finish report its outcome.
 * @p timeout_ms is the overall call deadline shared by every attempt (A6);
 * each attempt's grpc-timeout header carries the remaining time.
 */
cwist_grpc_channel_call *cwist_grpc_channel_call_start(cwist_grpc_channel *channel,
                                                       const char *method,
                                                       const void *request,
                                                       size_t request_len,
                                                       uint64_t timeout_ms);

int cwist_grpc_channel_call_recv(cwist_grpc_channel_call *call, cwist_grpc_message *out);
cwist_grpc_status_t cwist_grpc_channel_call_finish(cwist_grpc_channel_call *call,
                                                   const char **message);
void cwist_grpc_channel_call_cancel(cwist_grpc_channel_call *call);
void cwist_grpc_channel_call_destroy(cwist_grpc_channel_call *call);

/** Number of RPC attempts made (transparent retries excluded, gRFC A6). */
uint32_t cwist_grpc_channel_call_attempts(const cwist_grpc_channel_call *call);

/**
 * Unary convenience wrapper over cwist_grpc_channel_call_start(): buffers a
 * single response message into @p response (caller frees; NULL when the call
 * carried none) and returns the final grpc-status.  @p status_message
 * receives an owned copy of grpc-message (caller frees; may be NULL).
 */
cwist_grpc_status_t cwist_grpc_channel_unary(cwist_grpc_channel *channel,
                                             const char *method,
                                             const void *request, size_t request_len,
                                             uint64_t timeout_ms,
                                             uint8_t **response, size_t *response_len,
                                             char **status_message);

#ifdef __cplusplus
}
#endif

#endif
