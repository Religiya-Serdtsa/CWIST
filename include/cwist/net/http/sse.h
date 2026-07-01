/** @file sse.h @brief Server-Sent Events stream API. */
#ifndef CWIST_NET_HTTP_SSE_H
#define CWIST_NET_HTTP_SSE_H

#include <cwist/net/http/http.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cwist_sse_stream cwist_sse_stream_t;

/** A structured SSE event.  NULL fields are omitted; retry_ms < 0 omits retry. */
typedef struct cwist_sse_event {
    const char *event;
    const char *id;
    int retry_ms;
    const char *data;
} cwist_sse_event_t;

cwist_error_t cwist_sse_response_init(cwist_http_response *res);
cwist_error_t cwist_sse_response_event(cwist_http_response *res, const char *event,
                                       const char *id, int retry_ms, const char *data);
cwist_error_t cwist_sse_response_comment(cwist_http_response *res, const char *comment);
cwist_error_t cwist_sse_response_write(cwist_http_response *res, const cwist_sse_event_t *event);

/** Writes SSE response headers immediately and sets req->upgraded. */
cwist_sse_stream_t *cwist_sse_stream_open(cwist_http_request *req);
cwist_error_t cwist_sse_stream_send(cwist_sse_stream_t *stream, const char *event,
                                    const char *id, int retry_ms, const char *data);
cwist_error_t cwist_sse_stream_comment(cwist_sse_stream_t *stream, const char *comment);
cwist_error_t cwist_sse_stream_write(cwist_sse_stream_t *stream, const cwist_sse_event_t *event);
void cwist_sse_stream_close(cwist_sse_stream_t *stream);

/** Convenience forms for the common SSE payload styles. */
#define CWIST_SSE_EVENT(payload) ((cwist_sse_event_t){ .event = NULL, .id = NULL, .retry_ms = -1, .data = (payload) })
#define CWIST_SSE_NAMED(event_name, payload) ((cwist_sse_event_t){ .event = (event_name), .id = NULL, .retry_ms = -1, .data = (payload) })
#define CWIST_SSE_JSON(payload) CWIST_SSE_NAMED("message", (payload))
#define CWIST_SSE_RESPONSE_SEND(res, event) cwist_sse_response_write((res), &(event))
#define CWIST_SSE_STREAM_SEND(stream, event) cwist_sse_stream_write((stream), &(event))

#ifdef __cplusplus
}
#endif
#endif
