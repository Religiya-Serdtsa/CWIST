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
    cwist_grpc_unary_handler_func handler;
    void *user_ctx;
    struct cwist_grpc_route *next;
} cwist_grpc_route;

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

int cwist_app_grpc_unary(cwist_app *app,
                         const char *service,
                         const char *method,
                         cwist_grpc_unary_handler_func handler,
                         void *user_ctx) {
    if (!app || !service || !method || !handler) return -1;
    size_t service_len = strlen(service);
    size_t method_len = strlen(method);
    if (service_len == 0 || method_len == 0) return -1;

    size_t path_len = service_len + method_len + 3;
    char *path = (char *)cwist_alloc(path_len);
    if (!path) return -1;
    snprintf(path, path_len, "/%s/%s", service, method);

    cwist_grpc_route *existing = grpc_find_route(app, path);
    if (existing) {
        existing->handler = handler;
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
    route->handler = handler;
    route->user_ctx = user_ctx;
    route->next = (cwist_grpc_route *)app->grpc_routes;
    app->grpc_routes = route;

    cwist_app_post(app, path, grpc_dispatch_unary);
    return 0;
}

void cwist_grpc_routes_destroy(cwist_app *app) {
    if (!app) return;
    cwist_grpc_route *route = (cwist_grpc_route *)app->grpc_routes;
    while (route) {
        cwist_grpc_route *next = route->next;
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
        const char *path = route->path;
        if (!path || path[0] != '/') return -1;
        const char *method_sep = strrchr(path + 1, '/');
        if (!method_sep || method_sep == path + 1 || method_sep[1] == '\0') return -1;

        size_t service_len = (size_t)(method_sep - (path + 1));
        char *service = (char *)cwist_alloc(service_len + 1);
        if (!service) return -1;
        memcpy(service, path + 1, service_len);
        service[service_len] = '\0';

        int rc = cwist_app_grpc_unary(dst, service, method_sep + 1,
                                      route->handler, route->user_ctx);
        cwist_free(service);
        if (rc != 0) return -1;
        route = route->next;
    }
    return 0;
}
