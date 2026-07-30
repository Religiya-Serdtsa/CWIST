/**
 * @file grpc.c
 * @brief Unary gRPC over HTTP/2 support.
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/net/grpc/grpc.h>
#include <cwist/sys/app/app.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (message.compressed != 0) {
        cwist_grpc_set_error(res, CWIST_GRPC_UNIMPLEMENTED, "compressed gRPC messages are not supported");
        return;
    }

    route->handler(req, res, &message, route->user_ctx);
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
        if (messages[count].compressed != 0) {
            cwist_free(messages);
            cwist_grpc_set_error(res, CWIST_GRPC_UNIMPLEMENTED, "compressed gRPC messages are not supported");
            return;
        }
        count++;
    }

    res->status_code = CWIST_HTTP_OK;
    cwist_http_header_add(&res->headers, "content-type", "application/grpc");
    cwist_http_header_add(&res->headers, "grpc-accept-encoding", "identity");
    if (res->body) cwist_sstring_assign(res->body, "");

    cwist_grpc_stream stream = {
        .req = req,
        .res = res,
        .messages = messages,
        .message_count = count,
        .status = CWIST_GRPC_OK,
        .status_message = NULL,
        .closed = 0,
    };
    route->stream_handler(&stream, route->user_ctx);
    cwist_grpc_stream_close(&stream, stream.status, stream.status_message);
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
    cwist_http_header_add(&res->headers, "grpc-accept-encoding", "identity");

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
    if (!stream || !stream->res || !stream->res->body) return -1;
    uint8_t *frame = NULL;
    size_t frame_len = 0;
    if (cwist_grpc_encode_message(payload, payload_len, 0, &frame, &frame_len) != 0) {
        stream->status = CWIST_GRPC_INTERNAL;
        stream->status_message = "failed to encode gRPC stream message";
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
    if (!stream || !stream->res) return;
    if (stream->closed) return;
    stream->status = status;
    stream->status_message = message;
    stream->closed = 1;

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
