#include <cwist/net/http/sse.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    cwist_http_response *res = cwist_http_response_create();
    assert(res != NULL);
    assert(cwist_sse_response_init(res).error.err_i16 == 0);
    assert(cwist_sse_response_event(res, "update", "42", 1500, "first\nsecond").error.err_i16 == 0);
    assert(cwist_sse_response_comment(res, "heartbeat").error.err_i16 == 0);
    cwist_sse_event_t typed = CWIST_SSE_NAMED("typed", "payload");
    assert(cwist_sse_response_write(res, &typed).error.err_i16 == 0);
    assert(strcmp(cwist_http_header_get(res->headers, "Content-Type"), "text/event-stream; charset=utf-8") == 0);
    assert(strcmp(res->body->data, "id:42\nevent:update\nretry:1500\ndata:first\ndata:second\n\n:heartbeat\n\nevent:typed\ndata:payload\n\n") == 0);
    cwist_http_response_destroy(res);

    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    cwist_http_request *req = cwist_http_request_create();
    req->client_fd = pair[0];
    cwist_sse_stream_t *stream = cwist_sse_stream_open(req);
    assert(stream != NULL && req->upgraded);
    assert(cwist_sse_stream_send(stream, "message", "7", -1, "ok").error.err_i16 == 0);
    char received[512] = {0};
    ssize_t n = read(pair[1], received, sizeof(received) - 1);
    assert(n > 0);
    assert(strstr(received, "Content-Type: text/event-stream") != NULL);
    assert(strstr(received, "id:7\nevent:message\ndata:ok\n\n") != NULL);
    cwist_sse_event_t live_event = CWIST_SSE_EVENT("typed payload");
    assert(cwist_sse_stream_write(stream, &live_event).error.err_i16 == 0);
    cwist_sse_stream_close(stream);
    cwist_http_request_destroy(req);
    close(pair[0]); close(pair[1]);
    puts("All SSE tests passed.");
    return 0;
}
