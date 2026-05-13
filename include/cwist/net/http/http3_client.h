/**
 * @file http3_client.h
 * @brief HTTP/3 client using lsquic/BoringSSL.
 *
 * Provides a synchronous HTTP/3 client built on top of LiteSpeed's
 * lsquic library.  Supports 0-RTT session resumption, connection
 * migration, and QUIC datagrams.
 */

#ifndef __CWIST_HTTP3_CLIENT_H__
#define __CWIST_HTTP3_CLIENT_H__

#include <cwist/net/http/http.h>
#include <cwist/sys/err/cwist_err.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque HTTP/3 client handle.
 */
typedef struct cwist_http3_client cwist_http3_client;

/** @name Lifecycle */
/** @{ */

cwist_http3_client *cwist_http3_client_create(void);
void cwist_http3_client_destroy(cwist_http3_client *client);

/** @} */

/** @name Configuration */
/** @{ */

/**
 * @brief Set server address for subsequent requests.
 * @param client Client handle.
 * @param host Server hostname (used for SNI and :authority).
 * @param port Server UDP port.
 * @return 0 on success, -1 on failure.
 */
int cwist_http3_client_set_server(cwist_http3_client *client,
                                  const char *host, uint16_t port);

/**
 * @brief Load CA bundle for TLS certificate verification.
 * @param client Client handle.
 * @param ca_path Path to PEM CA bundle, or NULL to disable verification.
 * @return 0 on success, -1 on failure.
 */
int cwist_http3_client_set_ca_bundle(cwist_http3_client *client,
                                     const char *ca_path);

/**
 * @brief Set request timeout in milliseconds.
 * @param client Client handle.
 * @param timeout_ms Timeout, 0 for default (30000).
 */
void cwist_http3_client_set_timeout_ms(cwist_http3_client *client,
                                       int timeout_ms);

/**
 * @brief Enable or disable 0-RTT session resumption.
 *
 * When enabled, the client attempts to send application data in the
 * first flight if a previous session ticket is available.
 *
 * @param client Client handle.
 * @param enabled Non-zero to enable (default: 0).
 */
void cwist_http3_client_enable_0rtt(cwist_http3_client *client, int enabled);

/**
 * @brief Enable or disable QUIC datagram support (RFC 9221).
 *
 * When enabled, unreliable datagrams can be sent and received
 * via cwist_http3_client_send_datagram() / recv_datagram().
 *
 * @param client Client handle.
 * @param enabled Non-zero to enable (default: 0).
 */
void cwist_http3_client_enable_datagrams(cwist_http3_client *client,
                                         int enabled);

/** @} */

/** @name Request Execution */
/** @{ */

/**
 * @brief Perform a synchronous HTTP/3 request.
 *
 * Establishes a QUIC connection if none exists, sends the request,
 * and blocks until the complete response is received or the timeout
 * expires.
 *
 * @param client    Client handle.
 * @param path      Request path (e.g. "/index.html").
 * @param method    HTTP method enum.
 * @param headers   Optional request headers (may be NULL).
 * @param body      Optional request body (may be NULL).
 * @param body_len  Length of @p body.
 * @param out_response  Output pointer for parsed response.
 * @return CWIST error, err_i16 == 0 on success.
 */
cwist_error_t cwist_http3_client_request(cwist_http3_client *client,
                                         const char *path,
                                         cwist_http_method_t method,
                                         cwist_http_header_node *headers,
                                         const char *body,
                                         size_t body_len,
                                         cwist_http_response **out_response);

/** @} */

/** @name Datagrams (RFC 9221) */
/** @{ */

/**
 * @brief Send an unreliable QUIC datagram.
 *
 * Requires datagrams to be enabled.  The datagram is delivered on a
 * best-effort basis and may be silently dropped by the network.
 *
 * @param client Client handle.
 * @param data   Datagram payload.
 * @param len    Payload length.
 * @return 0 on success, -1 on failure.
 */
int cwist_http3_client_send_datagram(cwist_http3_client *client,
                                     const void *data, size_t len);

/**
 * @brief Receive an unreliable QUIC datagram.
 *
 * Non-blocking.  If no datagram is available, returns -1 with errno
 * set to EAGAIN.
 *
 * @param client Client handle.
 * @param buf    Buffer to store payload.
 * @param len    Buffer capacity.
 * @return Number of bytes received, or -1 on error.
 */
ssize_t cwist_http3_client_recv_datagram(cwist_http3_client *client,
                                         void *buf, size_t len);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
