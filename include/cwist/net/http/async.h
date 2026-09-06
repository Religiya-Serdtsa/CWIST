/**
 * @file async.h
 * @brief Deferred-response (async handler) API for CWIST.
 *
 * A route handler that cannot produce its response immediately calls
 * cwist_async_defer() and returns.  The framework then skips its normal
 * send + destroy steps and hands ownership of the request/response pair to
 * the returned cwist_async handle.  Any thread (scheduler job, NATS callback,
 * custom worker) later completes the exchange with exactly one of
 * cwist_async_respond(), cwist_async_respond_with(), or cwist_async_abort().
 *
 * Completion routing depends on the server mode:
 *  - C1M (event-driven) mode: the completion is posted to the owning reactor
 *    (cwist_reactor_post) and finished on the connection's owner thread, so
 *    reactor threads never block on foreign work.  While a request is
 *    deferred the connection is parked: pipelined bytes stay in the receive
 *    stash and are parsed only after the deferred response is sent, keeping
 *    responses ordered.  Keep-alive connections are re-armed afterwards.
 *  - Classic (thread pool) mode: the completing thread writes the response
 *    itself, then re-arms the connection on the pool (keep-alive) or closes
 *    it.
 *  - HTTP/2 mode: the completing thread enqueues the finished exchange on
 *    the owning connection's async queue and pokes its wake fd; the
 *    connection thread drains the queue and emits HEADERS/DATA itself, so
 *    HPACK state and flow control stay single-threaded.  Other streams on
 *    the connection keep being served while a response is deferred.
 *
 * Write-path note (v1): the client fd is O_NONBLOCK in C1M mode; completion
 * temporarily clears it and bounds the write with SO_SNDTIMEO (~5s), so a
 * slow client can occupy a reactor thread for that budget.  A
 * POLLOUT-resumable writer is future work (v2).
 *
 * Deferred responses bypass Big Dumb Reply learning and are never cached.
 */

#ifndef __CWIST_NET_HTTP_ASYNC_H__
#define __CWIST_NET_HTTP_ASYNC_H__

#include <cwist/net/http/http.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cwist_async cwist_async;

/**
 * @brief Defer the response for the current request.
 *
 * Call from inside a route handler and return immediately afterwards; the
 * handler must not touch @p req or @p res after this call.  Ownership of
 * both objects transfers to the returned handle.
 * @return Handle to complete later, or NULL on allocation failure (the
 * framework falls back to answering whatever the handler wrote).
 */
cwist_async *cwist_async_defer(cwist_http_request *req, cwist_http_response *res);

/**
 * @brief Answer with a 504 Gateway Timeout if the exchange is still pending
 * after @p ms milliseconds.  The timeout routes through the same completion
 * path as a normal response.  Default is no timeout (0).
 */
void cwist_async_set_timeout(cwist_async *a, uint64_t ms);

/**
 * @brief Complete the exchange with a simple body response.
 * Thread-safe and one-shot: the first of respond/respond_with/abort wins,
 * later calls return false.  @p body is copied.
 */
bool cwist_async_respond(cwist_async *a, cwist_http_status_t status, const char *content_type, const void *body, size_t len);

/**
 * @brief Complete the exchange with a caller-built response.
 * On success the framework takes ownership of @p res (destroyed after send).
 */
bool cwist_async_respond_with(cwist_async *a, cwist_http_response *res);

/**
 * @brief Complete the exchange with an error status and close the connection.
 */
bool cwist_async_abort(cwist_async *a, cwist_http_status_t status);

/** @brief Internal: acknowledge the dispatch-side handoff (app.c only). */
void cwist_async_dispatch_ack(cwist_async *a);

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_NET_HTTP_ASYNC_H__ */
