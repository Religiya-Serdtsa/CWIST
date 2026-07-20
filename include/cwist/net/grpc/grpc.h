/**
 * @file grpc.h
 * @brief gRPC over HTTP/2 helpers for CWIST.
 */

#ifndef __CWIST_GRPC_H__
#define __CWIST_GRPC_H__

#include <cwist/net/http/http.h>
#include <cwist/net/grpc/protobuf.h>
#include <stddef.h>
#include <stdint.h>

struct cwist_app;

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cwist_grpc_status {
    CWIST_GRPC_OK = 0,
    CWIST_GRPC_CANCELLED = 1,
    CWIST_GRPC_UNKNOWN = 2,
    CWIST_GRPC_INVALID_ARGUMENT = 3,
    CWIST_GRPC_DEADLINE_EXCEEDED = 4,
    CWIST_GRPC_NOT_FOUND = 5,
    CWIST_GRPC_ALREADY_EXISTS = 6,
    CWIST_GRPC_PERMISSION_DENIED = 7,
    CWIST_GRPC_RESOURCE_EXHAUSTED = 8,
    CWIST_GRPC_FAILED_PRECONDITION = 9,
    CWIST_GRPC_ABORTED = 10,
    CWIST_GRPC_OUT_OF_RANGE = 11,
    CWIST_GRPC_UNIMPLEMENTED = 12,
    CWIST_GRPC_INTERNAL = 13,
    CWIST_GRPC_UNAVAILABLE = 14,
    CWIST_GRPC_DATA_LOSS = 15,
    CWIST_GRPC_UNAUTHENTICATED = 16,
} cwist_grpc_status_t;

typedef struct cwist_grpc_message {
    uint8_t compressed;
    const uint8_t *data;
    size_t len;
} cwist_grpc_message;

typedef void (*cwist_grpc_unary_handler_func)(cwist_http_request *req,
                                               cwist_http_response *res,
                                               const cwist_grpc_message *message,
                                               void *user_ctx);

int cwist_grpc_decode_message(const void *frame,
                              size_t frame_len,
                              cwist_grpc_message *out);

int cwist_grpc_encode_message(const void *payload,
                              size_t payload_len,
                              uint8_t compressed,
                              uint8_t **out,
                              size_t *out_len);

void cwist_grpc_set_response(cwist_http_response *res,
                             cwist_grpc_status_t status,
                             const char *message,
                             const void *payload,
                             size_t payload_len);

void cwist_grpc_set_error(cwist_http_response *res,
                          cwist_grpc_status_t status,
                          const char *message);

int cwist_app_grpc_unary(struct cwist_app *app,
                         const char *service,
                         const char *method,
                         cwist_grpc_unary_handler_func handler,
                         void *user_ctx);

void cwist_grpc_routes_destroy(struct cwist_app *app);
int cwist_grpc_routes_clone(struct cwist_app *dst, const struct cwist_app *src);

#ifdef __cplusplus
}
#endif

#endif
