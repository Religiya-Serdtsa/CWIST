/**
 * @file http2.h
 * @brief HTTP/2 Protocol Definitions for CWIST.
 */

#ifndef __CWIST_HTTP2_H__
#define __CWIST_HTTP2_H__

#include <cwist/net/http/http.h>
#include <cwist/net/http/https.h>

/**
 * @brief Callback function type for handling HTTP/2 requests.
 *
 * @param user_ctx Opaque pointer for user context.
 * @param req Parsed HTTP request object.
 * @param res HTTP response object to be populated by the handler.
 */
typedef void (*cwist_http2_request_handler_func)(void *user_ctx,
                                                 cwist_http_request *req,
                                                 cwist_http_response *res);

/**
 * @brief Starts serving an HTTP/2 connection over an established HTTPS session.
 *
 * @param conn Pointer to the HTTPS connection that negotiated HTTP/2.
 * @param user_ctx Opaque user context passed to the handler callback.
 * @param handler Function to call when an HTTP/2 stream issues a request.
 * @return cwist_error_t Indicates success or connection error.
 */
cwist_error_t cwist_http2_serve_connection(cwist_https_connection *conn,
                                           void *user_ctx,
                                           cwist_http2_request_handler_func handler);

#endif
