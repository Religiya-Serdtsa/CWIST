#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/sse.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

struct cwist_sse_stream { int fd; int closed; pthread_mutex_t mutex; };

static cwist_error_t sse_error(int value) {
    return (cwist_error_t){ .errtype = CWIST_ERR_INT16, .error.err_i16 = value };
}

static int append_field(cwist_sstring *out, const char *name, const char *value) {
    const char *line = value ? value : "";
    do {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len && line[len - 1] == '\r') len--;
        if (cwist_sstring_append(out, name).error.err_i16 ||
            cwist_sstring_append(out, ":").error.err_i16 ||
            (len && cwist_sstring_append_len(out, line, len).error.err_i16) ||
            cwist_sstring_append(out, "\n").error.err_i16) return -1;
        line = end ? end + 1 : NULL;
    } while (line);
    return 0;
}

static cwist_sstring *format_event(const char *event, const char *id, int retry_ms,
                                   const char *data, int is_comment) {
    cwist_sstring *frame = cwist_sstring_create();
    if (!frame) return NULL;
    int failed = is_comment ? append_field(frame, "", data) : 0;
    if (!is_comment && id) failed |= append_field(frame, "id", id);
    if (!is_comment && event) failed |= append_field(frame, "event", event);
    if (!is_comment && retry_ms >= 0) {
        char retry[32];
        snprintf(retry, sizeof(retry), "%d", retry_ms);
        failed |= append_field(frame, "retry", retry);
    }
    if (!is_comment) failed |= append_field(frame, "data", data);
    if (failed || cwist_sstring_append(frame, "\n").error.err_i16) {
        cwist_sstring_destroy(frame);
        return NULL;
    }
    return frame;
}

cwist_error_t cwist_sse_response_init(cwist_http_response *res) {
    if (!res || cwist_http_header_add(&res->headers, "Content-Type", "text/event-stream; charset=utf-8").error.err_i16 ||
        cwist_http_header_add(&res->headers, "Cache-Control", "no-cache").error.err_i16 ||
        cwist_http_header_add(&res->headers, "X-Accel-Buffering", "no").error.err_i16) return sse_error(-1);
    res->keep_alive = true;
    return sse_error(0);
}

cwist_error_t cwist_sse_response_event(cwist_http_response *res, const char *event,
                                       const char *id, int retry_ms, const char *data) {
    if (!res || retry_ms < -1) return sse_error(-1);
    cwist_sstring *frame = format_event(event, id, retry_ms, data, 0);
    if (!frame) return sse_error(-1);
    cwist_error_t result = cwist_sstring_append_len(res->body, frame->data, frame->size);
    cwist_sstring_destroy(frame);
    return result;
}

cwist_error_t cwist_sse_response_comment(cwist_http_response *res, const char *comment) {
    if (!res) return sse_error(-1);
    cwist_sstring *frame = format_event(NULL, NULL, -1, comment, 1);
    if (!frame) return sse_error(-1);
    cwist_error_t result = cwist_sstring_append_len(res->body, frame->data, frame->size);
    cwist_sstring_destroy(frame);
    return result;
}

cwist_error_t cwist_sse_response_write(cwist_http_response *res, const cwist_sse_event_t *event) {
    if (!event) return sse_error(-1);
    return cwist_sse_response_event(res, event->event, event->id, event->retry_ms, event->data);
}

static int send_all(int fd, const char *data, size_t len) {
    while (len) {
        ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        data += n; len -= (size_t)n;
    }
    return 0;
}

cwist_sse_stream_t *cwist_sse_stream_open(cwist_http_request *req) {
    static const char headers[] = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nCache-Control: no-cache\r\nX-Accel-Buffering: no\r\nConnection: keep-alive\r\n\r\n";
    if (!req || req->client_fd < 0 || req->upgraded || send_all(req->client_fd, headers, sizeof(headers) - 1)) return NULL;
    cwist_sse_stream_t *stream = cwist_alloc(sizeof(*stream));
    if (!stream) return NULL;
    stream->fd = req->client_fd; stream->closed = 0;
    pthread_mutex_init(&stream->mutex, NULL);
    req->upgraded = true;
    return stream;
}

cwist_error_t cwist_sse_stream_send(cwist_sse_stream_t *stream, const char *event,
                                    const char *id, int retry_ms, const char *data) {
    if (!stream || retry_ms < -1) return sse_error(-1);
    cwist_sstring *frame = format_event(event, id, retry_ms, data, 0);
    if (!frame) return sse_error(-1);
    pthread_mutex_lock(&stream->mutex);
    int rc = stream->closed ? -1 : send_all(stream->fd, frame->data, frame->size);
    if (rc) stream->closed = 1;
    pthread_mutex_unlock(&stream->mutex);
    cwist_sstring_destroy(frame);
    return sse_error(rc);
}

cwist_error_t cwist_sse_stream_comment(cwist_sse_stream_t *stream, const char *comment) {
    if (!stream) return sse_error(-1);
    cwist_sstring *frame = format_event(NULL, NULL, -1, comment, 1);
    if (!frame) return sse_error(-1);
    pthread_mutex_lock(&stream->mutex);
    int rc = stream->closed ? -1 : send_all(stream->fd, frame->data, frame->size);
    if (rc) stream->closed = 1;
    pthread_mutex_unlock(&stream->mutex);
    cwist_sstring_destroy(frame);
    return sse_error(rc);
}

cwist_error_t cwist_sse_stream_write(cwist_sse_stream_t *stream, const cwist_sse_event_t *event) {
    if (!event) return sse_error(-1);
    return cwist_sse_stream_send(stream, event->event, event->id, event->retry_ms, event->data);
}

void cwist_sse_stream_close(cwist_sse_stream_t *stream) {
    if (!stream) return;
    pthread_mutex_destroy(&stream->mutex);
    cwist_free(stream);
}
