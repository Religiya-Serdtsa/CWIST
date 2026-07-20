#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/test_client.h>
#include <cwist/net/grpc/grpc.h>
#include <cwist/core/mem/alloc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void echo_unary(cwist_http_request *req,
                       cwist_http_response *res,
                       const cwist_grpc_message *message,
                       void *user_ctx) {
    (void)req;
    const char *prefix = (const char *)user_ctx;
    cwist_pb_reader reader;
    cwist_pb_reader_init(&reader, message->data, message->len);
    cwist_pb_field field;
    const char *input = NULL;
    size_t input_len = 0;
    while (cwist_pb_read_field(&reader, &field) > 0) {
        if (field.number == 1 && field.wire_type == CWIST_PB_LEN) {
            input = (const char *)field.bytes;
            input_len = field.len;
        }
    }
    assert(input != NULL);

    char out[128];
    int n = snprintf(out, sizeof(out), "%s:%.*s",
                     prefix ? prefix : "echo",
                     (int)input_len,
                     input);
    assert(n > 0);

    cwist_pb_writer writer;
    cwist_pb_writer_init(&writer);
    assert(cwist_pb_write_string_field(&writer, 1, out) == 0);
    assert(cwist_pb_write_uint64_field(&writer, 2, 7) == 0);
    cwist_grpc_set_response(res, CWIST_GRPC_OK, NULL, writer.data, writer.len);
    cwist_pb_writer_free(&writer);
}

static void decode_response(cwist_http_response *res, const char *expected) {
    assert(res != NULL);
    assert(res->status_code == CWIST_HTTP_OK);
    assert(strcmp(cwist_http_header_get(res->headers, "content-type"), "application/grpc") == 0);
    assert(strcmp(cwist_http_header_get(res->headers, "grpc-status"), "0") == 0);

    cwist_grpc_message msg;
    assert(cwist_grpc_decode_message(res->body->data, res->body->size, &msg) == 0);
    assert(msg.compressed == 0);

    cwist_pb_reader reader;
    cwist_pb_reader_init(&reader, msg.data, msg.len);
    cwist_pb_field field;
    int saw_text = 0;
    int saw_code = 0;
    while (cwist_pb_read_field(&reader, &field) > 0) {
        if (field.number == 1 && field.wire_type == CWIST_PB_LEN) {
            assert(field.len == strlen(expected));
            assert(memcmp(field.bytes, expected, field.len) == 0);
            saw_text = 1;
        } else if (field.number == 2 && field.wire_type == CWIST_PB_VARINT) {
            assert(field.varint == 7);
            saw_code = 1;
        }
    }
    assert(saw_text && saw_code);
}

int main(void) {
    cwist_pb_writer request_pb;
    cwist_pb_writer_init(&request_pb);
    assert(cwist_pb_write_string_field(&request_pb, 1, "ping") == 0);
    assert(cwist_pb_write_bool_field(&request_pb, 2, 1) == 0);

    uint8_t *frame = NULL;
    size_t frame_len = 0;
    assert(cwist_grpc_encode_message(request_pb.data, request_pb.len, 0, &frame, &frame_len) == 0);
    assert(frame_len == request_pb.len + 5);

    cwist_grpc_message decoded;
    assert(cwist_grpc_decode_message(frame, frame_len, &decoded) == 0);
    assert(decoded.compressed == 0);
    assert(decoded.len == request_pb.len);
    assert(memcmp(decoded.data, request_pb.data, request_pb.len) == 0);
    assert(cwist_grpc_decode_message(frame, frame_len - 1, &decoded) != 0);

    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    assert(cwist_app_grpc_unary(app, "cwist.test.Echo", "Say", echo_unary, "reply") == 0);

    cwist_test_client *client = cwist_test_client_create(app);
    assert(client != NULL);

    cwist_test_client_request_options opts = {
        .body = (const char *)frame,
        .body_len = frame_len,
        .content_type = "application/grpc",
    };
    cwist_http_response *res = cwist_test_client_request_ex(client, CWIST_HTTP_POST,
                                                            "/cwist.test.Echo/Say",
                                                            &opts);
    decode_response(res, "reply:ping");
    cwist_http_response_destroy(res);

    opts.content_type = "application/octet-stream";
    res = cwist_test_client_request_ex(client, CWIST_HTTP_POST,
                                       "/cwist.test.Echo/Say",
                                       &opts);
    assert(res != NULL);
    assert(strcmp(cwist_http_header_get(res->headers, "grpc-status"), "3") == 0);
    cwist_http_response_destroy(res);

    opts.content_type = "application/grpc";
    static const unsigned char malformed[] = { 0, 0, 0, 0, 5, 'b', 'a', 'd' };
    opts.body = (const char *)malformed;
    opts.body_len = sizeof(malformed);
    res = cwist_test_client_request_ex(client, CWIST_HTTP_POST,
                                       "/cwist.test.Echo/Say",
                                       &opts);
    assert(res != NULL);
    assert(strcmp(cwist_http_header_get(res->headers, "grpc-status"), "3") == 0);
    cwist_http_response_destroy(res);

    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    cwist_free(frame);
    cwist_pb_writer_free(&request_pb);

    printf("test_grpc: OK\n");
    return 0;
}
