/**
 * @file grpc.c
 * @brief Unary gRPC over HTTP/2 support.
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/net/grpc/grpc.h>
#include <cwist/sys/app/app.h>
#include <cwist/core/mem/alloc.h>
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <zlib.h>

typedef struct cwist_grpc_route {
    char *path;
    int streaming;
    int builtin;
    cwist_grpc_unary_handler_func handler;
    cwist_grpc_stream_handler_func stream_handler;
    void *user_ctx;
    struct cwist_grpc_route *next;
} cwist_grpc_route;

typedef struct cwist_grpc_health_state {
    char *service;
    int serving;
    struct cwist_grpc_health_state *next;
} cwist_grpc_health_state;

static int grpc_register_route(cwist_app *app, const char *service, const char *method,
                               int streaming, cwist_grpc_unary_handler_func unary_handler,
                               cwist_grpc_stream_handler_func stream_handler, void *user_ctx);

/* --- Incremental HTTP/2 streaming sessions --- */

typedef struct grpc_qnode {
    uint8_t *data;
    size_t len;
    struct grpc_qnode *next;
} grpc_qnode;

typedef struct cwist_grpc_session cwist_grpc_session;

static void grpc_session_send_trailers(cwist_grpc_session *session,
                                       cwist_grpc_status_t status,
                                       const char *message);

static int grpc_content_type_is_grpc(const char *content_type) {
    if (!content_type) return 0;
    return strncmp(content_type, "application/grpc", 16) == 0;
}

static cwist_grpc_route *grpc_find_route(cwist_app *app, const char *path) {
    if (!app || !path) return NULL;
    cwist_grpc_route *route = (cwist_grpc_route *)app->grpc_routes;
    while (route) {
        if (route->path && strcmp(route->path, path) == 0) return route;
        route = route->next;
    }
    return NULL;
}

static uint64_t grpc_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static const char *grpc_header_get(cwist_http_request *req, const char *key) {
    if (!req || !key) return NULL;
    for (cwist_http_header_node *h = req->headers; h; h = h->next) {
        if (h->key && h->key->data && h->value && h->value->data &&
            strcasecmp(h->key->data, key) == 0)
            return h->value->data;
    }
    return NULL;
}

int cwist_grpc_parse_timeout(const char *value, uint64_t *out_ms) {
    if (!value || !out_ms) return -1;
    size_t len = strlen(value);
    if (len < 2 || len > 9) return -1; /* up to 8 digits + unit */
    char unit = value[len - 1];
    uint64_t to_ms_num = 1, to_ms_den = 1;
    switch (unit) {
        case 'H': to_ms_num = 3600000; break;
        case 'M': to_ms_num = 60000; break;
        case 'S': to_ms_num = 1000; break;
        case 'm': to_ms_num = 1; break;
        case 'u': to_ms_num = 1; to_ms_den = 1000; break;
        case 'n': to_ms_num = 1; to_ms_den = 1000000; break;
        default: return -1;
    }
    uint64_t v = 0;
    for (size_t i = 0; i + 1 < len; i++) {
        if (!isdigit((unsigned char)value[i])) return -1;
        v = v * 10 + (uint64_t)(value[i] - '0');
    }
    /* Round sub-millisecond units up so a positive timeout never becomes 0. */
    uint64_t ms = (v * to_ms_num + to_ms_den - 1) / to_ms_den;
    if (v > 0 && ms == 0) ms = 1;
    *out_ms = ms;
    return 0;
}

/* grpc-encoding the request declared: 0 = identity/absent, 1 = gzip,
 * -1 = unsupported (must be rejected with UNIMPLEMENTED). */
static int grpc_request_encoding(cwist_http_request *req) {
    const char *enc = grpc_header_get(req, "grpc-encoding");
    if (!enc || strcmp(enc, "identity") == 0) return 0;
    if (strcmp(enc, "gzip") == 0) return 1;
    return -1;
}

static int grpc_gzip_inflate(const uint8_t *in, size_t in_len,
                             uint8_t **out, size_t *out_len) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return -1;
    size_t cap = in_len * 3 + 1024;
    if (cap < 8192) cap = 8192;
    uint8_t *buf = (uint8_t *)cwist_alloc(cap);
    if (!buf) {
        inflateEnd(&zs);
        return -1;
    }
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)in_len;
    size_t used = 0;
    int zrc = Z_OK;
    while (zrc == Z_OK) {
        if (used == cap) {
            cap *= 2;
            uint8_t *nb = (uint8_t *)cwist_realloc(buf, cap);
            if (!nb) {
                cwist_free(buf);
                inflateEnd(&zs);
                return -1;
            }
            buf = nb;
        }
        zs.next_out = buf + used;
        zs.avail_out = (uInt)(cap - used);
        zrc = inflate(&zs, Z_NO_FLUSH);
        used = cap - zs.avail_out;
    }
    inflateEnd(&zs);
    if (zrc != Z_STREAM_END) {
        cwist_free(buf);
        return -1;
    }
    *out = buf;
    *out_len = used;
    return 0;
}

/* Decompress a compressed-flag message according to grpc-encoding.
 * On success the message points at an owned buffer returned in @p owned
 * (caller frees).  Returns 0 on success, -1 when the encoding is
 * unsupported or the payload is corrupt. */
static int grpc_message_inflate(cwist_http_request *req, cwist_grpc_message *message,
                                uint8_t **owned) {
    *owned = NULL;
    if (!message->compressed) return 0;
    if (grpc_request_encoding(req) != 1) return -1;
    uint8_t *plain = NULL;
    size_t plain_len = 0;
    if (grpc_gzip_inflate(message->data, message->len, &plain, &plain_len) != 0)
        return -1;
    message->data = plain;
    message->len = plain_len;
    message->compressed = 0;
    *owned = plain;
    return 0;
}

static int grpc_b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int grpc_base64_decode(const char *in, size_t in_len,
                              uint8_t *out, size_t out_cap, size_t *out_len) {
    uint32_t acc = 0;
    int bits = 0;
    size_t used = 0;
    for (size_t i = 0; i < in_len; i++) {
        if (in[i] == '=') break;
        int v = grpc_b64_val((unsigned char)in[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (used >= out_cap) return -2;
            out[used++] = (uint8_t)(acc >> bits);
        }
    }
    *out_len = used;
    return 0;
}

const char *cwist_grpc_metadata_get(cwist_http_request *req, const char *key) {
    return grpc_header_get(req, key);
}

int cwist_grpc_metadata_get_binary(cwist_http_request *req, const char *key,
                                   uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!req || !key || !out_len) return -1;
    size_t key_len = strlen(key);
    if (key_len < 4 || strcmp(key + key_len - 4, "-bin") != 0) return -1;
    const char *value = grpc_header_get(req, key);
    if (!value) return -1;
    return grpc_base64_decode(value, strlen(value), out, out_cap, out_len);
}

static void grpc_dispatch_unary(cwist_http_request *req, cwist_http_response *res) {
    if (!req || !res || !req->app || !req->path || !req->path->data) return;

    cwist_grpc_route *route = grpc_find_route(req->app, req->path->data);
    if (!route || !route->handler) {
        cwist_grpc_set_error(res, CWIST_GRPC_UNIMPLEMENTED, "gRPC method not registered");
        return;
    }

    const char *ct = cwist_http_header_get(req->headers, "content-type");
    if (!grpc_content_type_is_grpc(ct)) {
        ct = cwist_http_header_get(req->headers, "Content-Type");
    }
    if (!grpc_content_type_is_grpc(ct)) {
        cwist_grpc_set_error(res, CWIST_GRPC_INVALID_ARGUMENT, "content-type must be application/grpc");
        return;
    }

    if (!req->body || !req->body->data) {
        cwist_grpc_set_error(res, CWIST_GRPC_INVALID_ARGUMENT, "missing gRPC request body");
        return;
    }

    cwist_grpc_message message;
    if (cwist_grpc_decode_message(req->body->data, req->body->size, &message) != 0) {
        cwist_grpc_set_error(res, CWIST_GRPC_INVALID_ARGUMENT, "malformed gRPC message frame");
        return;
    }
    if (grpc_request_encoding(req) < 0) {
        cwist_grpc_set_error(res, CWIST_GRPC_UNIMPLEMENTED, "unsupported grpc-encoding");
        return;
    }
    uint8_t *inflated = NULL;
    if (grpc_message_inflate(req, &message, &inflated) != 0) {
        cwist_grpc_set_error(res, CWIST_GRPC_UNIMPLEMENTED, "unsupported compressed gRPC message");
        return;
    }

    route->handler(req, res, &message, route->user_ctx);
    cwist_free(inflated);
}

static int grpc_validate_request(cwist_http_request *req, cwist_http_response *res) {
    const char *ct = cwist_http_header_get(req->headers, "content-type");
    if (!grpc_content_type_is_grpc(ct)) {
        ct = cwist_http_header_get(req->headers, "Content-Type");
    }
    if (!grpc_content_type_is_grpc(ct)) {
        cwist_grpc_set_error(res, CWIST_GRPC_INVALID_ARGUMENT, "content-type must be application/grpc");
        return -1;
    }
    if (!req->body || !req->body->data) {
        cwist_grpc_set_error(res, CWIST_GRPC_INVALID_ARGUMENT, "missing gRPC request body");
        return -1;
    }
    if (grpc_request_encoding(req) < 0) {
        cwist_grpc_set_error(res, CWIST_GRPC_UNIMPLEMENTED, "unsupported grpc-encoding");
        return -1;
    }
    return 0;
}

static void grpc_dispatch_stream(cwist_http_request *req, cwist_http_response *res) {
    if (!req || !res || !req->app || !req->path || !req->path->data) return;

    cwist_grpc_route *route = grpc_find_route(req->app, req->path->data);
    if (!route || !route->stream_handler) {
        cwist_grpc_set_error(res, CWIST_GRPC_UNIMPLEMENTED, "gRPC stream method not registered");
        return;
    }
    if (grpc_validate_request(req, res) != 0) return;

    size_t offset = 0;
    size_t cap = 4;
    size_t count = 0;
    cwist_grpc_message *messages = (cwist_grpc_message *)cwist_alloc(cap * sizeof(*messages));
    if (!messages) {
        cwist_grpc_set_error(res, CWIST_GRPC_RESOURCE_EXHAUSTED, "failed to allocate gRPC stream messages");
        return;
    }

    while (offset < req->body->size) {
        if (count == cap) {
            cap *= 2;
            cwist_grpc_message *next = (cwist_grpc_message *)cwist_realloc(messages, cap * sizeof(*messages));
            if (!next) {
                cwist_free(messages);
                cwist_grpc_set_error(res, CWIST_GRPC_RESOURCE_EXHAUSTED, "failed to grow gRPC stream messages");
                return;
            }
            messages = next;
        }
        if (cwist_grpc_decode_next_message(req->body->data, req->body->size,
                                           &offset, &messages[count]) != 0) {
            cwist_free(messages);
            cwist_grpc_set_error(res, CWIST_GRPC_INVALID_ARGUMENT, "malformed gRPC stream frame");
            return;
        }
        count++;
    }

    /* Decompress any compressed-flag messages when grpc-encoding: gzip. */
    uint8_t **owned = NULL;
    int has_compressed = 0;
    for (size_t i = 0; i < count; i++)
        if (messages[i].compressed) { has_compressed = 1; break; }
    if (has_compressed) {
        owned = (uint8_t **)cwist_alloc(count * sizeof(*owned));
        if (!owned) {
            cwist_free(messages);
            cwist_grpc_set_error(res, CWIST_GRPC_RESOURCE_EXHAUSTED, "failed to allocate inflate state");
            return;
        }
        memset(owned, 0, count * sizeof(*owned));
        for (size_t i = 0; i < count; i++) {
            if (grpc_message_inflate(req, &messages[i], &owned[i]) != 0) {
                for (size_t j = 0; j < count; j++) cwist_free(owned[j]);
                cwist_free(owned);
                cwist_free(messages);
                cwist_grpc_set_error(res, CWIST_GRPC_UNIMPLEMENTED, "unsupported compressed gRPC message");
                return;
            }
        }
    }

    uint64_t deadline_ms = 0;
    const char *timeout = grpc_header_get(req, "grpc-timeout");
    if (timeout) {
        uint64_t timeout_ms = 0;
        if (cwist_grpc_parse_timeout(timeout, &timeout_ms) == 0)
            deadline_ms = grpc_now_ms() + timeout_ms;
    }

    res->status_code = CWIST_HTTP_OK;
    cwist_http_header_add(&res->headers, "content-type", "application/grpc");
    cwist_http_header_add(&res->headers, "grpc-accept-encoding", "gzip, identity");
    if (res->body) cwist_sstring_assign(res->body, "");

    cwist_grpc_stream stream = {
        .req = req,
        .res = res,
        .messages = messages,
        .message_count = count,
        .status = CWIST_GRPC_OK,
        .status_message = NULL,
        .closed = 0,
        .session = NULL,
        .cancelled = 0,
        .deadline_ms = deadline_ms,
    };
    route->stream_handler(&stream, route->user_ctx);
    cwist_grpc_stream_close(&stream, stream.status, stream.status_message);
    if (owned) {
        for (size_t i = 0; i < count; i++) cwist_free(owned[i]);
        cwist_free(owned);
    }
    cwist_free(messages);
}

int cwist_grpc_decode_message(const void *frame,
                              size_t frame_len,
                              cwist_grpc_message *out) {
    if (!frame || !out || frame_len < 5) return -1;
    const uint8_t *p = (const uint8_t *)frame;
    uint32_t len = ((uint32_t)p[1] << 24) |
                   ((uint32_t)p[2] << 16) |
                   ((uint32_t)p[3] << 8) |
                   (uint32_t)p[4];
    if ((size_t)len > frame_len - 5) return -1;
    if ((size_t)len != frame_len - 5) return -1;

    out->compressed = p[0];
    out->data = p + 5;
    out->len = (size_t)len;
    return 0;
}

int cwist_grpc_decode_next_message(const void *frames,
                                   size_t frames_len,
                                   size_t *offset,
                                   cwist_grpc_message *out) {
    if (!frames || !offset || !out || *offset > frames_len) return -1;
    size_t pos = *offset;
    if (frames_len - pos < 5) return -1;

    const uint8_t *p = (const uint8_t *)frames + pos;
    uint32_t len = ((uint32_t)p[1] << 24) |
                   ((uint32_t)p[2] << 16) |
                   ((uint32_t)p[3] << 8) |
                   (uint32_t)p[4];
    if ((size_t)len > frames_len - pos - 5) return -1;

    out->compressed = p[0];
    out->data = p + 5;
    out->len = (size_t)len;
    *offset = pos + 5 + (size_t)len;
    return 0;
}

int cwist_grpc_encode_message(const void *payload,
                              size_t payload_len,
                              uint8_t compressed,
                              uint8_t **out,
                              size_t *out_len) {
    if (!out || !out_len || (payload_len > 0 && !payload)) return -1;
    if (payload_len > UINT32_MAX) return -1;

    uint8_t *buf = (uint8_t *)cwist_alloc(payload_len + 5);
    if (!buf) return -1;
    buf[0] = compressed ? 1 : 0;
    buf[1] = (uint8_t)((payload_len >> 24) & 0xff);
    buf[2] = (uint8_t)((payload_len >> 16) & 0xff);
    buf[3] = (uint8_t)((payload_len >> 8) & 0xff);
    buf[4] = (uint8_t)(payload_len & 0xff);
    if (payload_len > 0) memcpy(buf + 5, payload, payload_len);

    *out = buf;
    *out_len = payload_len + 5;
    return 0;
}

void cwist_grpc_decoder_init(cwist_grpc_decoder *decoder, size_t max_message_size) {
    if (!decoder) return;
    memset(decoder, 0, sizeof(*decoder));
    decoder->max_message_size = max_message_size ? max_message_size : (16u * 1024u * 1024u);
}

void cwist_grpc_decoder_destroy(cwist_grpc_decoder *decoder) {
    if (!decoder) return;
    cwist_free(decoder->payload);
    memset(decoder, 0, sizeof(*decoder));
}

int cwist_grpc_decoder_feed(cwist_grpc_decoder *decoder, const void *data, size_t len,
                            cwist_grpc_message_callback callback, void *ctx) {
    if (!decoder || (!data && len) || !callback) return -1;
    const uint8_t *input = data;
    while (len) {
        if (decoder->header_len < sizeof(decoder->header)) {
            size_t take = sizeof(decoder->header) - decoder->header_len;
            if (take > len) take = len;
            memcpy(decoder->header + decoder->header_len, input, take);
            decoder->header_len += take;
            input += take;
            len -= take;
            if (decoder->header_len < sizeof(decoder->header)) continue;
            decoder->compressed = decoder->header[0];
            decoder->payload_len = ((size_t)decoder->header[1] << 24) |
                                   ((size_t)decoder->header[2] << 16) |
                                   ((size_t)decoder->header[3] << 8) |
                                   (size_t)decoder->header[4];
            if (decoder->compressed > 1 || decoder->payload_len > decoder->max_message_size)
                return -1;
            if (decoder->payload_len) {
                decoder->payload = cwist_alloc(decoder->payload_len);
                if (!decoder->payload) return -1;
            }
        }
        size_t take = decoder->payload_len - decoder->payload_used;
        if (take > len) take = len;
        if (take) memcpy(decoder->payload + decoder->payload_used, input, take);
        decoder->payload_used += take;
        input += take;
        len -= take;
        if (decoder->payload_used != decoder->payload_len) continue;
        cwist_grpc_message message = { decoder->compressed, decoder->payload, decoder->payload_len };
        if (callback(ctx, &message) != 0) return -1;
        cwist_free(decoder->payload);
        decoder->payload = NULL;
        decoder->payload_len = decoder->payload_used = decoder->header_len = 0;
    }
    return 0;
}

void cwist_grpc_set_response(cwist_http_response *res,
                             cwist_grpc_status_t status,
                             const char *message,
                             const void *payload,
                             size_t payload_len) {
    if (!res) return;

    res->status_code = CWIST_HTTP_OK;
    cwist_http_header_add(&res->headers, "content-type", "application/grpc");
    cwist_http_header_add(&res->headers, "grpc-accept-encoding", "gzip, identity");

    char status_buf[16];
    snprintf(status_buf, sizeof(status_buf), "%d", (int)status);
    cwist_http_header_add(&res->headers, "grpc-status", status_buf);
    if (message) {
        cwist_http_header_add(&res->headers, "grpc-message", message);
    }

    uint8_t *frame = NULL;
    size_t frame_len = 0;
    if (cwist_grpc_encode_message(payload, payload_len, 0, &frame, &frame_len) == 0) {
        cwist_sstring_assign_len(res->body, (char *)frame, frame_len);
        cwist_free(frame);
    } else {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "");
        cwist_http_header_add(&res->headers, "grpc-status", "13");
        cwist_http_header_add(&res->headers, "grpc-message", "failed to encode gRPC response");
    }
}

void cwist_grpc_set_error(cwist_http_response *res,
                          cwist_grpc_status_t status,
                          const char *message) {
    cwist_grpc_set_response(res, status, message, NULL, 0);
}

int cwist_grpc_stream_send(cwist_grpc_stream *stream,
                           const void *payload,
                           size_t payload_len) {
    if (!stream) return -1;
    uint8_t *frame = NULL;
    size_t frame_len = 0;
    if (cwist_grpc_encode_message(payload, payload_len, 0, &frame, &frame_len) != 0) {
        stream->status = CWIST_GRPC_INTERNAL;
        stream->status_message = "failed to encode gRPC stream message";
        return -1;
    }
    if (stream->session) {
        /* Transport-backed stream: emit a DATA frame immediately. */
        if (!stream->write_frame ||
            stream->write_frame(stream->write_frame_ctx, frame, frame_len, 0) != 0) {
            cwist_free(frame);
            stream->status = CWIST_GRPC_UNAVAILABLE;
            stream->status_message = "failed to write gRPC DATA frame";
            return -1;
        }
        cwist_free(frame);
        return 0;
    }
    if (!stream->res || !stream->res->body) {
        cwist_free(frame);
        return -1;
    }
    if (stream->write_frame && stream->write_frame(stream->write_frame_ctx, frame, frame_len, 0) != 0) {
        cwist_free(frame);
        stream->status = CWIST_GRPC_UNAVAILABLE;
        stream->status_message = "failed to write gRPC DATA frame";
        return -1;
    }
    cwist_error_t err = cwist_sstring_append_len(stream->res->body, (char *)frame, frame_len);
    cwist_free(frame);
    if (err.error.err_i16 != 0) {
        stream->status = CWIST_GRPC_INTERNAL;
        stream->status_message = "failed to append gRPC stream message";
        return -1;
    }
    return 0;
}

void cwist_grpc_stream_set_writer(cwist_grpc_stream *stream,
                                  int (*write_frame)(void *, const uint8_t *, size_t, int),
                                  void *ctx) {
    if (!stream) return;
    stream->write_frame = write_frame;
    stream->write_frame_ctx = ctx;
}

void cwist_grpc_stream_close(cwist_grpc_stream *stream,
                             cwist_grpc_status_t status,
                             const char *message) {
    if (!stream) return;
    if (stream->closed) return;
    stream->status = status;
    stream->status_message = message;
    stream->closed = 1;

    if (stream->session) {
        grpc_session_send_trailers(stream->session, status, message);
        return;
    }
    if (!stream->res) return;

    char status_buf[16];
    snprintf(status_buf, sizeof(status_buf), "%d", (int)status);
    cwist_http_header_add(&stream->res->headers, "grpc-status", status_buf);
    if (message) {
        cwist_http_header_add(&stream->res->headers, "grpc-message", message);
    }
}

static int grpc_health_status(const cwist_grpc_health_state *state, const char *service) {
    for (const cwist_grpc_health_state *it = state; it; it = it->next)
        if (it->service && strcmp(it->service, service ? service : "") == 0)
            return it->serving ? 1 : 2;
    return service && *service ? 3 : 1; /* SERVICE_UNKNOWN, or overall SERVING */
}

static void grpc_health_reply(cwist_http_response *res, cwist_grpc_health_state *state,
                              const cwist_grpc_message *message) {
    const char *service = "";
    char *owned = NULL;
    cwist_pb_reader reader;
    cwist_pb_reader_init(&reader, message->data, message->len);
    cwist_pb_field field;
    while (cwist_pb_read_field(&reader, &field) > 0)
        if (field.number == 1 && field.wire_type == CWIST_PB_LEN) {
            owned = cwist_alloc(field.len + 1);
            if (!owned) { cwist_grpc_set_error(res, CWIST_GRPC_RESOURCE_EXHAUSTED, "health allocation failed"); return; }
            memcpy(owned, field.bytes, field.len); owned[field.len] = '\0'; service = owned; break;
        }
    cwist_pb_writer writer;
    cwist_pb_writer_init(&writer);
    if (cwist_pb_write_uint64_field(&writer, 1, (uint64_t)grpc_health_status(state, service)) != 0)
        cwist_grpc_set_error(res, CWIST_GRPC_INTERNAL, "health encoding failed");
    else
        cwist_grpc_set_response(res, CWIST_GRPC_OK, NULL, writer.data, writer.len);
    cwist_pb_writer_free(&writer);
    cwist_free(owned);
}

static void grpc_health_check(cwist_http_request *req, cwist_http_response *res,
                              const cwist_grpc_message *message, void *ctx) {
    (void)req;
    grpc_health_reply(res, ctx, message);
}

static void grpc_health_watch(cwist_grpc_stream *stream, void *ctx) {
    cwist_grpc_message empty = { 0, NULL, 0 };
    const cwist_grpc_message *message = stream->message_count ? &stream->messages[0] : &empty;
    cwist_http_response *res = stream->res;
    grpc_health_reply(res, ctx, message);
}

int cwist_app_grpc_health(cwist_app *app) {
    if (!app) return -1;
    cwist_grpc_health_state *state = cwist_alloc(sizeof(*state));
    if (!state) return -1;
    memset(state, 0, sizeof(*state));
    if (grpc_register_route(app, "grpc.health.v1.Health", "Check", 0,
                            grpc_health_check, NULL, state) != 0 ||
        grpc_register_route(app, "grpc.health.v1.Health", "Watch", 1,
                            NULL, grpc_health_watch, state) != 0) {
        cwist_free(state);
        return -1;
    }
    cwist_grpc_route *route = grpc_find_route(app, "/grpc.health.v1.Health/Check");
    if (route) route->builtin = 1;
    route = grpc_find_route(app, "/grpc.health.v1.Health/Watch");
    if (route) route->builtin = 2;
    return 0;
}

int cwist_app_grpc_health_set_status(cwist_app *app, const char *service, int serving) {
    cwist_grpc_route *route = grpc_find_route(app, "/grpc.health.v1.Health/Check");
    if (!route || !route->user_ctx || !service) return -1;
    cwist_grpc_health_state *state = route->user_ctx;
    for (cwist_grpc_health_state *it = state; it; it = it->next)
        if (it->service && strcmp(it->service, service) == 0) { it->serving = !!serving; return 0; }
    cwist_grpc_health_state *item = cwist_alloc(sizeof(*item));
    if (!item) return -1;
    item->service = cwist_alloc(strlen(service) + 1);
    if (!item->service) { cwist_free(item); return -1; }
    strcpy(item->service, service); item->serving = !!serving; item->next = state->next; state->next = item;
    return 0;
}

static int grpc_reflection_append_service(cwist_pb_writer *response, const char *service) {
    cwist_pb_writer item, list;
    cwist_pb_writer_init(&item); cwist_pb_writer_init(&list);
    int rc = cwist_pb_write_string_field(&item, 1, service) ||
             cwist_pb_write_bytes_field(&list, 1, item.data, item.len) ||
             cwist_pb_write_bytes_field(response, 6, list.data, list.len);
    cwist_pb_writer_free(&item); cwist_pb_writer_free(&list);
    return rc ? -1 : 0;
}

static void grpc_reflection_info(cwist_grpc_stream *stream, void *ctx) {
    (void)ctx;
    cwist_pb_writer response;
    cwist_pb_writer_init(&response);
    cwist_grpc_route *route = (cwist_grpc_route *)stream->req->app->grpc_routes;
    for (; route; route = route->next) {
        if (!route->path || route->path[0] != '/') continue;
        const char *slash = strrchr(route->path + 1, '/');
        if (!slash) continue;
        size_t len = (size_t)(slash - route->path - 1);
        char *service = cwist_alloc(len + 1);
        if (!service) { stream->status = CWIST_GRPC_RESOURCE_EXHAUSTED; break; }
        memcpy(service, route->path + 1, len); service[len] = '\0';
        if (grpc_reflection_append_service(&response, service) != 0) stream->status = CWIST_GRPC_INTERNAL;
        cwist_free(service);
        if (stream->status != CWIST_GRPC_OK) break;
    }
    if (stream->status == CWIST_GRPC_OK) cwist_grpc_stream_send(stream, response.data, response.len);
    cwist_pb_writer_free(&response);
}

int cwist_app_grpc_reflection(cwist_app *app) {
    if (grpc_register_route(app, "grpc.reflection.v1alpha.ServerReflection", "ServerReflectionInfo",
                            1, NULL, grpc_reflection_info, NULL) != 0) return -1;
    cwist_grpc_route *route = grpc_find_route(app,
        "/grpc.reflection.v1alpha.ServerReflection/ServerReflectionInfo");
    if (route) route->builtin = 3;
    return 0;
}

static int grpc_register_route(cwist_app *app,
                               const char *service,
                               const char *method,
                               int streaming,
                               cwist_grpc_unary_handler_func unary_handler,
                               cwist_grpc_stream_handler_func stream_handler,
                               void *user_ctx) {
    if (!app || !service || !method) return -1;
    if ((!streaming && !unary_handler) || (streaming && !stream_handler)) return -1;
    size_t service_len = strlen(service);
    size_t method_len = strlen(method);
    if (service_len == 0 || method_len == 0) return -1;

    size_t path_len = service_len + method_len + 3;
    char *path = (char *)cwist_alloc(path_len);
    if (!path) return -1;
    snprintf(path, path_len, "/%s/%s", service, method);

    cwist_grpc_route *existing = grpc_find_route(app, path);
    if (existing) {
        existing->streaming = streaming;
        existing->handler = unary_handler;
        existing->stream_handler = stream_handler;
        existing->user_ctx = user_ctx;
        cwist_free(path);
        return 0;
    }

    cwist_grpc_route *route = (cwist_grpc_route *)cwist_alloc(sizeof(*route));
    if (!route) {
        cwist_free(path);
        return -1;
    }
    route->path = path;
    route->streaming = streaming;
    route->handler = unary_handler;
    route->stream_handler = stream_handler;
    route->user_ctx = user_ctx;
    route->next = (cwist_grpc_route *)app->grpc_routes;
    app->grpc_routes = route;

    cwist_app_post(app, path, streaming ? grpc_dispatch_stream : grpc_dispatch_unary);
    return 0;
}

int cwist_app_grpc_unary(cwist_app *app,
                         const char *service,
                         const char *method,
                         cwist_grpc_unary_handler_func handler,
                         void *user_ctx) {
    return grpc_register_route(app, service, method, 0, handler, NULL, user_ctx);
}

int cwist_app_grpc_stream(cwist_app *app,
                          const char *service,
                          const char *method,
                          cwist_grpc_stream_handler_func handler,
                          void *user_ctx) {
    return grpc_register_route(app, service, method, 1, NULL, handler, user_ctx);
}

void cwist_grpc_routes_destroy(cwist_app *app) {
    if (!app) return;
    cwist_grpc_route *route = (cwist_grpc_route *)app->grpc_routes;
    while (route) {
        cwist_grpc_route *next = route->next;
        if (route->builtin == 1 && route->user_ctx) {
            cwist_grpc_health_state *state = route->user_ctx;
            while (state) {
                cwist_grpc_health_state *state_next = state->next;
                cwist_free(state->service);
                cwist_free(state);
                state = state_next;
            }
        }
        cwist_free(route->path);
        cwist_free(route);
        route = next;
    }
    app->grpc_routes = NULL;
}

int cwist_grpc_routes_clone(cwist_app *dst, const cwist_app *src) {
    if (!dst || !src) return -1;
    const cwist_grpc_route *route = (const cwist_grpc_route *)src->grpc_routes;
    while (route) {
        if (route->builtin == 1) {
            if (cwist_app_grpc_health(dst) != 0) return -1;
            const cwist_grpc_health_state *state = route->user_ctx;
            for (const cwist_grpc_health_state *it = state; it; it = it->next)
                if (it->service && cwist_app_grpc_health_set_status(dst, it->service, it->serving) != 0)
                    return -1;
            route = route->next;
            continue;
        }
        if (route->builtin == 2) { route = route->next; continue; }
        if (route->builtin == 3) {
            if (cwist_app_grpc_reflection(dst) != 0) return -1;
            route = route->next;
            continue;
        }
        const char *path = route->path;
        if (!path || path[0] != '/') return -1;
        const char *method_sep = strrchr(path + 1, '/');
        if (!method_sep || method_sep == path + 1 || method_sep[1] == '\0') return -1;

        size_t service_len = (size_t)(method_sep - (path + 1));
        char *service = (char *)cwist_alloc(service_len + 1);
        if (!service) return -1;
        memcpy(service, path + 1, service_len);
        service[service_len] = '\0';

        int rc = route->streaming
            ? cwist_app_grpc_stream(dst, service, method_sep + 1,
                                    route->stream_handler, route->user_ctx)
            : cwist_app_grpc_unary(dst, service, method_sep + 1,
                                   route->handler, route->user_ctx);
        cwist_free(service);
        if (rc != 0) return -1;
        route = route->next;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Incremental HTTP/2 streaming session engine                         */
/* ------------------------------------------------------------------ */

struct cwist_grpc_session {
    pthread_mutex_t mu;    /* inbound queue, eof, cancelled */
    pthread_cond_t cond;
    pthread_mutex_t wmu;   /* outbound writes and h2s lifetime */
    cwist_h2_stream *h2s;  /* NULL once the transport detaches */
    void *conn_ctx;        /* cwist_grpc_h2_conn_ctx */
    cwist_grpc_decoder decoder;
    int encoding;          /* 0 identity, 1 gzip */
    grpc_qnode *qhead;
    grpc_qnode *qtail;
    int eof;
    int cancelled;
    int trailers_sent;
    int decode_failed;
    cwist_grpc_stream stream; /* public stream object (embeds req) */
    uint8_t *recv_buf;        /* backing store for the last recv message */
    cwist_grpc_stream_handler_func handler;
    void *user_ctx;
    atomic_int refs;          /* transport side + handler thread */
    struct cwist_grpc_session *next;
};

typedef struct cwist_grpc_h2_conn_ctx {
    cwist_app *app;
    pthread_mutex_t mu;             /* sessions list + refs */
    int refs;                       /* connection + one per session */
    cwist_grpc_session *sessions;
} cwist_grpc_h2_conn_ctx;

static void grpc_conn_ctx_release(cwist_grpc_h2_conn_ctx *ctx) {
    int last;
    pthread_mutex_lock(&ctx->mu);
    last = (--ctx->refs == 0);
    pthread_mutex_unlock(&ctx->mu);
    if (last) {
        pthread_mutex_destroy(&ctx->mu);
        cwist_free(ctx);
    }
}

static void grpc_session_release(cwist_grpc_session *session) {
    if (atomic_fetch_sub(&session->refs, 1) != 1) return;
    cwist_grpc_h2_conn_ctx *ctx = session->conn_ctx;
    pthread_mutex_lock(&ctx->mu);
    cwist_grpc_session **pp = &ctx->sessions;
    while (*pp && *pp != session) pp = &(*pp)->next;
    if (*pp) *pp = session->next;
    pthread_mutex_unlock(&ctx->mu);
    grpc_conn_ctx_release(ctx);

    cwist_grpc_decoder_destroy(&session->decoder);
    while (session->qhead) {
        grpc_qnode *next = session->qhead->next;
        cwist_free(session->qhead->data);
        cwist_free(session->qhead);
        session->qhead = next;
    }
    cwist_free(session->recv_buf);
    if (session->stream.req) cwist_http_request_destroy(session->stream.req);
    pthread_mutex_destroy(&session->mu);
    pthread_cond_destroy(&session->cond);
    pthread_mutex_destroy(&session->wmu);
    cwist_free(session);
}

/* Queue a decoded inbound message for the handler thread. */
static int grpc_session_push(cwist_grpc_session *session, uint8_t *data, size_t len) {
    grpc_qnode *node = (grpc_qnode *)cwist_alloc(sizeof(*node));
    if (!node) return -1;
    node->data = data;
    node->len = len;
    node->next = NULL;
    pthread_mutex_lock(&session->mu);
    if (session->cancelled || session->eof) {
        pthread_mutex_unlock(&session->mu);
        cwist_free(data);
        cwist_free(node);
        return -1;
    }
    if (session->qtail) session->qtail->next = node;
    else session->qhead = node;
    session->qtail = node;
    pthread_cond_signal(&session->cond);
    pthread_mutex_unlock(&session->mu);
    return 0;
}

static int grpc_session_decoded(void *ctx, const cwist_grpc_message *message) {
    cwist_grpc_session *session = ctx;
    uint8_t *copy;
    size_t len;
    if (message->compressed) {
        if (session->encoding != 1) return -1; /* rejected by on_data */
        if (grpc_gzip_inflate(message->data, message->len, &copy, &len) != 0)
            return -1;
    } else {
        copy = (uint8_t *)cwist_alloc(message->len ? message->len : 1);
        if (!copy) return -1;
        if (message->len) memcpy(copy, message->data, message->len);
        len = message->len;
    }
    if (grpc_session_push(session, copy, len) != 0) return -1;
    return 0;
}

/* Fail the call: cancel the handler and emit error trailers. */
static void grpc_session_fail(cwist_grpc_session *session,
                              cwist_grpc_status_t status,
                              const char *message) {
    pthread_mutex_lock(&session->mu);
    session->cancelled = 1;
    session->stream.cancelled = 1;
    session->stream.status = status;
    session->stream.status_message = message;
    pthread_cond_broadcast(&session->cond);
    pthread_mutex_unlock(&session->mu);
    grpc_session_send_trailers(session, status, message);
}

static void grpc_session_send_trailers(cwist_grpc_session *session,
                                       cwist_grpc_status_t status,
                                       const char *message) {
    char status_buf[16];
    snprintf(status_buf, sizeof(status_buf), "%d", (int)status);
    cwist_http2_header pairs[2];
    size_t count = 0;
    pairs[count].name = "grpc-status";
    pairs[count].value = status_buf;
    count++;
    if (message) {
        pairs[count].name = "grpc-message";
        pairs[count].value = message;
        count++;
    }
    pthread_mutex_lock(&session->wmu);
    if (session->h2s && !session->trailers_sent) {
        if (cwist_http2_stream_send_trailers(session->h2s, pairs, count) == 0)
            session->trailers_sent = 1;
        else
            session->trailers_sent = 1; /* transport broken; do not retry */
    }
    pthread_mutex_unlock(&session->wmu);
}

static int grpc_session_write_frame(void *ctx, const uint8_t *frame,
                                    size_t frame_len, int end_stream) {
    (void)end_stream;
    cwist_grpc_session *session = ctx;
    pthread_mutex_lock(&session->wmu);
    int rc = -1;
    if (session->h2s && !session->trailers_sent)
        rc = cwist_http2_stream_send_data(session->h2s, frame, frame_len);
    pthread_mutex_unlock(&session->wmu);
    return rc;
}

static void *grpc_session_thread(void *arg) {
    cwist_grpc_session *session = arg;
    session->handler(&session->stream, session->user_ctx);
    if (!session->stream.closed) {
        cwist_grpc_stream_close(&session->stream,
                                session->stream.status,
                                session->stream.status_message);
    }
    grpc_session_release(session);
    return NULL;
}

int cwist_grpc_stream_recv(cwist_grpc_stream *stream, cwist_grpc_message *out) {
    if (!stream || !out) return -1;

    if (!stream->session) {
        /* Buffered path: pop the pre-decoded message array. */
        if (stream->deadline_ms && grpc_now_ms() >= stream->deadline_ms) {
            stream->cancelled = 1;
            return -1;
        }
        if (stream->recv_pos >= stream->message_count) return 0;
        *out = stream->messages[stream->recv_pos++];
        return 1;
    }

    cwist_grpc_session *session = stream->session;
    pthread_mutex_lock(&session->mu);
    for (;;) {
        if (session->qhead) {
            grpc_qnode *node = session->qhead;
            session->qhead = node->next;
            if (!session->qhead) session->qtail = NULL;
            pthread_mutex_unlock(&session->mu);
            cwist_free(session->recv_buf);
            session->recv_buf = node->data;
            out->compressed = 0;
            out->data = node->data;
            out->len = node->len;
            cwist_free(node);
            return 1;
        }
        if (session->cancelled) {
            pthread_mutex_unlock(&session->mu);
            return -1;
        }
        if (session->eof) {
            pthread_mutex_unlock(&session->mu);
            return 0;
        }
        if (stream->deadline_ms) {
            uint64_t now = grpc_now_ms();
            if (now >= stream->deadline_ms) {
                session->cancelled = 1;
                stream->cancelled = 1;
                /* Report the real outcome so a handler that simply closes
                 * with stream->status still emits DEADLINE_EXCEEDED. */
                stream->status = CWIST_GRPC_DEADLINE_EXCEEDED;
                stream->status_message = "deadline exceeded";
                pthread_mutex_unlock(&session->mu);
                return -1;
            }
            uint64_t left = stream->deadline_ms - now;
            struct timespec ts;
            struct timespec rt;
            clock_gettime(CLOCK_REALTIME, &rt);
            ts.tv_sec = rt.tv_sec + (time_t)(left / 1000);
            ts.tv_nsec = rt.tv_nsec + (long)((left % 1000) * 1000000);
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&session->cond, &session->mu, &ts);
        } else {
            pthread_cond_wait(&session->cond, &session->mu);
        }
    }
}

int cwist_grpc_stream_cancelled(cwist_grpc_stream *stream) {
    if (!stream) return 0;
    if (stream->cancelled) return 1;
    if (stream->session) {
        cwist_grpc_session *session = stream->session;
        pthread_mutex_lock(&session->mu);
        int cancelled = session->cancelled;
        pthread_mutex_unlock(&session->mu);
        return cancelled;
    }
    return 0;
}

uint64_t cwist_grpc_stream_deadline_remaining_ms(cwist_grpc_stream *stream) {
    if (!stream || !stream->deadline_ms) return UINT64_MAX;
    uint64_t now = grpc_now_ms();
    return now >= stream->deadline_ms ? 0 : stream->deadline_ms - now;
}

/* --- HTTP/2 stream hooks --- */

static void *grpc_h2_on_conn_open(void *user_ctx) {
    cwist_grpc_h2_conn_ctx *ctx = (cwist_grpc_h2_conn_ctx *)cwist_alloc(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->app = (cwist_app *)user_ctx;
    ctx->refs = 1;
    ctx->sessions = NULL;
    pthread_mutex_init(&ctx->mu, NULL);
    return ctx;
}

static void grpc_h2_on_conn_close(void *conn_ctx) {
    if (conn_ctx) grpc_conn_ctx_release(conn_ctx);
}

static void *grpc_h2_on_headers(void *conn_ctx, cwist_http_request *req,
                                cwist_h2_stream *stream) {
    cwist_grpc_h2_conn_ctx *ctx = conn_ctx;
    if (!ctx || !ctx->app || !req || !req->path || !req->path->data) return NULL;

    cwist_grpc_route *route = grpc_find_route(ctx->app, req->path->data);
    if (!route || !route->stream_handler || route->builtin) return NULL;

    const char *ct = grpc_header_get(req, "content-type");
    if (!grpc_content_type_is_grpc(ct)) return NULL;

    int encoding = grpc_request_encoding(req);
    if (encoding < 0) return NULL; /* buffered dispatch replies UNIMPLEMENTED */

    cwist_grpc_session *session = (cwist_grpc_session *)cwist_alloc(sizeof(*session));
    if (!session) return NULL;
    memset(session, 0, sizeof(*session));
    pthread_mutex_init(&session->mu, NULL);
    pthread_cond_init(&session->cond, NULL);
    pthread_mutex_init(&session->wmu, NULL);
    session->h2s = stream;
    session->conn_ctx = ctx;
    session->encoding = encoding;
    cwist_grpc_decoder_init(&session->decoder, 0);
    atomic_store(&session->refs, 2); /* transport + handler thread */

    session->stream.req = req;
    session->stream.res = NULL;
    session->stream.messages = NULL;
    session->stream.message_count = 0;
    session->stream.status = CWIST_GRPC_OK;
    session->stream.status_message = NULL;
    session->stream.closed = 0;
    session->stream.write_frame = grpc_session_write_frame;
    session->stream.write_frame_ctx = session;
    session->stream.session = session;
    session->stream.cancelled = 0;
    session->stream.recv_pos = 0;
    const char *timeout = grpc_header_get(req, "grpc-timeout");
    if (timeout) {
        uint64_t timeout_ms = 0;
        if (cwist_grpc_parse_timeout(timeout, &timeout_ms) == 0)
            session->stream.deadline_ms = grpc_now_ms() + timeout_ms;
    }
    session->handler = route->stream_handler;
    session->user_ctx = route->user_ctx;

    /* Initial response headers go out immediately so the client can start
     * receiving before the first message. */
    static const cwist_http2_header resp_headers[] = {
        { "content-type", "application/grpc" },
        { "grpc-accept-encoding", "gzip, identity" },
    };
    if (cwist_http2_stream_send_headers(stream, 200, resp_headers, 2, 0) != 0) {
        pthread_mutex_destroy(&session->mu);
        pthread_cond_destroy(&session->cond);
        pthread_mutex_destroy(&session->wmu);
        cwist_grpc_decoder_destroy(&session->decoder);
        cwist_free(session);
        return NULL;
    }

    pthread_mutex_lock(&ctx->mu);
    ctx->refs++;
    session->next = ctx->sessions;
    ctx->sessions = session;
    pthread_mutex_unlock(&ctx->mu);

    pthread_t tid;
    if (pthread_create(&tid, NULL, grpc_session_thread, session) != 0) {
        pthread_mutex_lock(&ctx->mu);
        ctx->refs--;
        cwist_grpc_session **pp = &ctx->sessions;
        while (*pp && *pp != session) pp = &(*pp)->next;
        if (*pp) *pp = session->next;
        pthread_mutex_unlock(&ctx->mu);
        pthread_mutex_destroy(&session->mu);
        pthread_cond_destroy(&session->cond);
        pthread_mutex_destroy(&session->wmu);
        cwist_grpc_decoder_destroy(&session->decoder);
        cwist_free(session);
        return NULL;
    }
    pthread_detach(tid);
    return session;
}

static int grpc_h2_on_data(void *conn_ctx, void *stream_ctx,
                           const unsigned char *data, size_t len, int end_stream) {
    (void)conn_ctx;
    cwist_grpc_session *session = stream_ctx;
    if (!session) return 1;

    if (data && len > 0) {
        if (cwist_grpc_decoder_feed(&session->decoder, data, len,
                                    grpc_session_decoded, session) != 0) {
            if (session->encoding != 1) {
                /* Compressed flag set without grpc-encoding: gzip. */
                grpc_session_fail(session, CWIST_GRPC_UNIMPLEMENTED,
                                  "compressed gRPC message without grpc-encoding: gzip");
            } else {
                grpc_session_fail(session, CWIST_GRPC_INVALID_ARGUMENT,
                                  "malformed gRPC message frame");
            }
            return 0; /* trailers carry the error; the poll sweep reaps us */
        }
    }
    if (end_stream) {
        pthread_mutex_lock(&session->mu);
        session->eof = 1;
        pthread_cond_broadcast(&session->cond);
        pthread_mutex_unlock(&session->mu);
    }
    return 0;
}

static void grpc_h2_on_cancel(void *conn_ctx, void *stream_ctx) {
    (void)conn_ctx;
    cwist_grpc_session *session = stream_ctx;
    if (!session) return;
    pthread_mutex_lock(&session->mu);
    session->cancelled = 1;
    session->stream.cancelled = 1;
    pthread_cond_broadcast(&session->cond);
    pthread_mutex_unlock(&session->mu);
}

static int grpc_h2_on_poll(void *conn_ctx, void *stream_ctx) {
    (void)conn_ctx;
    cwist_grpc_session *session = stream_ctx;
    if (!session) return 1;

    /* Deadline enforcement: cancel the handler and emit DEADLINE_EXCEEDED
     * trailers so the client learns the outcome. */
    if (session->stream.deadline_ms && !session->trailers_sent &&
        grpc_now_ms() >= session->stream.deadline_ms) {
        pthread_mutex_lock(&session->mu);
        session->cancelled = 1;
        session->stream.cancelled = 1;
        pthread_cond_broadcast(&session->cond);
        pthread_mutex_unlock(&session->mu);
        grpc_session_send_trailers(session, CWIST_GRPC_DEADLINE_EXCEEDED,
                                   "deadline exceeded");
    }
    return session->trailers_sent;
}

static uint64_t grpc_h2_next_deadline_ms(void *conn_ctx) {
    cwist_grpc_h2_conn_ctx *ctx = conn_ctx;
    if (!ctx) return 0;
    uint64_t nearest = 0;
    pthread_mutex_lock(&ctx->mu);
    for (cwist_grpc_session *s = ctx->sessions; s; s = s->next) {
        if (s->trailers_sent || !s->stream.deadline_ms) continue;
        if (!nearest || s->stream.deadline_ms < nearest)
            nearest = s->stream.deadline_ms;
    }
    pthread_mutex_unlock(&ctx->mu);
    return nearest;
}

static void grpc_h2_on_close(void *conn_ctx, void *stream_ctx) {
    (void)conn_ctx;
    cwist_grpc_session *session = stream_ctx;
    if (!session) return;
    /* Transport is gone: detach so handler-thread sends fail fast. */
    pthread_mutex_lock(&session->wmu);
    session->h2s = NULL;
    pthread_mutex_unlock(&session->wmu);
    pthread_mutex_lock(&session->mu);
    session->cancelled = 1;
    session->stream.cancelled = 1;
    pthread_cond_broadcast(&session->cond);
    pthread_mutex_unlock(&session->mu);
    grpc_session_release(session);
}

static const cwist_http2_stream_hooks grpc_h2_hooks = {
    .on_conn_open = grpc_h2_on_conn_open,
    .on_conn_close = grpc_h2_on_conn_close,
    .on_headers = grpc_h2_on_headers,
    .on_data = grpc_h2_on_data,
    .on_cancel = grpc_h2_on_cancel,
    .on_poll = grpc_h2_on_poll,
    .next_deadline_ms = grpc_h2_next_deadline_ms,
    .on_close = grpc_h2_on_close,
};

const cwist_http2_stream_hooks *cwist_grpc_http2_hooks(void) {
    return &grpc_h2_hooks;
}
