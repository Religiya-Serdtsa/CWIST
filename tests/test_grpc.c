#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/test_client.h>
#include <cwist/net/grpc/grpc.h>
#include <cwist/core/mem/alloc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

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

static void echo_stream(cwist_grpc_stream *stream, void *user_ctx) {
    const char *prefix = (const char *)user_ctx;
    for (size_t i = 0; i < stream->message_count; i++) {
        cwist_pb_reader reader;
        cwist_pb_reader_init(&reader, stream->messages[i].data, stream->messages[i].len);
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
        int n = snprintf(out, sizeof(out), "%s-%zu:%.*s",
                         prefix ? prefix : "stream",
                         i + 1,
                         (int)input_len,
                         input);
        assert(n > 0);

        cwist_pb_writer writer;
        cwist_pb_writer_init(&writer);
        assert(cwist_pb_write_string_field(&writer, 1, out) == 0);
        assert(cwist_grpc_stream_send(stream, writer.data, writer.len) == 0);
        cwist_pb_writer_free(&writer);
    }
    cwist_grpc_stream_close(stream, CWIST_GRPC_OK, NULL);
}

static void meta_unary(cwist_http_request *req,
                       cwist_http_response *res,
                       const cwist_grpc_message *message,
                       void *user_ctx) {
    (void)message;
    (void)user_ctx;
    /* Metadata normalization: case-insensitive textual lookup, and a
     * base64-decoded *-bin value. */
    const char *meta = cwist_grpc_metadata_get(req, "x-meta-echo");
    assert(meta != NULL && strcmp(meta, "hello-meta") == 0);
    uint8_t bin[16];
    size_t bin_len = 0;
    assert(cwist_grpc_metadata_get_binary(req, "trace-bin", bin, sizeof(bin), &bin_len) == 0);
    assert(bin_len == 3 && memcmp(bin, "\x01\x02\x03", 3) == 0);

    cwist_pb_writer writer;
    cwist_pb_writer_init(&writer);
    assert(cwist_pb_write_string_field(&writer, 1, meta) == 0);
    cwist_grpc_set_response(res, CWIST_GRPC_OK, NULL, writer.data, writer.len);
    cwist_pb_writer_free(&writer);
}

static void recv_stream(cwist_grpc_stream *stream, void *user_ctx) {
    (void)user_ctx;
    /* Exercise the buffered-path recv API: it must replay every decoded
     * message in order, then report EOF. */
    cwist_grpc_message msg;
    size_t seen = 0;
    int rc;
    while ((rc = cwist_grpc_stream_recv(stream, &msg)) == 1) {
        cwist_pb_reader reader;
        cwist_pb_reader_init(&reader, msg.data, msg.len);
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
        cwist_pb_writer writer;
        cwist_pb_writer_init(&writer);
        char out[128];
        snprintf(out, sizeof(out), "recv-%zu:%.*s", seen + 1, (int)input_len, input);
        assert(cwist_pb_write_string_field(&writer, 1, out) == 0);
        assert(cwist_grpc_stream_send(stream, writer.data, writer.len) == 0);
        cwist_pb_writer_free(&writer);
        seen++;
    }
    assert(rc == 0);
    assert(seen == stream->message_count);
    assert(cwist_grpc_stream_deadline_remaining_ms(stream) == UINT64_MAX);
    cwist_grpc_stream_close(stream, CWIST_GRPC_OK, NULL);
}

/* gzip-compress with zlib (client side of the compression test). */
static void gzip_compress(const uint8_t *in, size_t in_len,
                          uint8_t **out, size_t *out_len) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    assert(deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                        16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    size_t cap = deflateBound(&zs, in_len) + 32;
    uint8_t *buf = cwist_alloc(cap);
    assert(buf != NULL);
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)in_len;
    zs.next_out = buf;
    zs.avail_out = (uInt)cap;
    assert(deflate(&zs, Z_FINISH) == Z_STREAM_END);
    *out_len = cap - zs.avail_out;
    *out = buf;
    deflateEnd(&zs);
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

static void append_grpc_string_frame(cwist_sstring *body, const char *value) {
    cwist_pb_writer pb;
    cwist_pb_writer_init(&pb);
    assert(cwist_pb_write_string_field(&pb, 1, value) == 0);

    uint8_t *frame = NULL;
    size_t frame_len = 0;
    assert(cwist_grpc_encode_message(pb.data, pb.len, 0, &frame, &frame_len) == 0);
    assert(cwist_sstring_append_len(body, (char *)frame, frame_len).error.err_i16 == 0);

    cwist_free(frame);
    cwist_pb_writer_free(&pb);
}

static void assert_stream_response(cwist_http_response *res,
                                   const char **expected,
                                   size_t expected_count) {
    assert(res != NULL);
    assert(strcmp(cwist_http_header_get(res->headers, "grpc-status"), "0") == 0);

    size_t offset = 0;
    size_t seen = 0;
    while (offset < res->body->size) {
        cwist_grpc_message msg;
        assert(cwist_grpc_decode_next_message(res->body->data, res->body->size,
                                              &offset, &msg) == 0);
        assert(seen < expected_count);
        cwist_pb_reader reader;
        cwist_pb_reader_init(&reader, msg.data, msg.len);
        cwist_pb_field field;
        int saw_text = 0;
        while (cwist_pb_read_field(&reader, &field) > 0) {
            if (field.number == 1 && field.wire_type == CWIST_PB_LEN) {
                assert(field.len == strlen(expected[seen]));
                assert(memcmp(field.bytes, expected[seen], field.len) == 0);
                saw_text = 1;
            }
        }
        assert(saw_text);
        seen++;
    }
    assert(seen == expected_count);
}

static int decoder_count(void *ctx, const cwist_grpc_message *message) {
    size_t *count = ctx;
    assert(message->compressed == 0);
    (*count)++;
    return 0;
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

    cwist_grpc_decoder decoder;
    cwist_grpc_decoder_init(&decoder, 1024);
    size_t decoded_count = 0;
    assert(cwist_grpc_decoder_feed(&decoder, frame, 2, decoder_count, &decoded_count) == 0);
    assert(cwist_grpc_decoder_feed(&decoder, frame + 2, frame_len - 2, decoder_count, &decoded_count) == 0);
    assert(decoded_count == 1);
    cwist_grpc_decoder_destroy(&decoder);

    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    assert(cwist_app_grpc_unary(app, "cwist.test.Echo", "Say", echo_unary, "reply") == 0);
    assert(cwist_app_grpc_unary(app, "cwist.test.Echo", "Meta", meta_unary, NULL) == 0);
    assert(cwist_app_grpc_stream(app, "cwist.test.Echo", "Chat", echo_stream, "chunk") == 0);
    assert(cwist_app_grpc_stream(app, "cwist.test.Echo", "Recv", recv_stream, NULL) == 0);
    assert(cwist_app_grpc_health(app) == 0);
    assert(cwist_app_grpc_health_set_status(app, "cwist.test.Echo", 1) == 0);
    assert(cwist_app_grpc_reflection(app) == 0);

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

    cwist_sstring *stream_body = cwist_sstring_create();
    assert(stream_body != NULL);
    append_grpc_string_frame(stream_body, "one");
    append_grpc_string_frame(stream_body, "two");

    opts.body = stream_body->data;
    opts.body_len = stream_body->size;
    opts.content_type = "application/grpc";
    res = cwist_test_client_request_ex(client, CWIST_HTTP_POST,
                                       "/cwist.test.Echo/Chat",
                                       &opts);
    const char *expected_stream[] = { "chunk-1:one", "chunk-2:two" };
    assert_stream_response(res, expected_stream, 2);
    cwist_http_response_destroy(res);
    cwist_sstring_destroy(stream_body);

    uint8_t *empty_frame = NULL;
    size_t empty_frame_len = 0;
    assert(cwist_grpc_encode_message(NULL, 0, 0, &empty_frame, &empty_frame_len) == 0);
    opts.body = (const char *)empty_frame;
    opts.body_len = empty_frame_len;
    res = cwist_test_client_request_ex(client, CWIST_HTTP_POST,
                                       "/grpc.health.v1.Health/Check", &opts);
    cwist_grpc_message health_message;
    assert(cwist_grpc_decode_message(res->body->data, res->body->size, &health_message) == 0);
    cwist_pb_reader health_reader;
    cwist_pb_reader_init(&health_reader, health_message.data, health_message.len);
    cwist_pb_field health_field;
    assert(cwist_pb_read_field(&health_reader, &health_field) > 0);
    assert(health_field.number == 1 && health_field.varint == 1);
    cwist_http_response_destroy(res);
    cwist_free(empty_frame);

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

    /* Metadata normalization over the buffered path. */
    static const cwist_test_client_kv meta_headers[] = {
        { "X-Meta-Echo", "hello-meta" },  /* case-insensitive lookup */
        { "trace-bin", "AQID" },          /* base64 of 0x01 0x02 0x03 */
    };
    opts.body = (const char *)frame;
    opts.body_len = frame_len;
    opts.headers = meta_headers;
    opts.header_count = 2;
    res = cwist_test_client_request_ex(client, CWIST_HTTP_POST,
                                       "/cwist.test.Echo/Meta", &opts);
    assert(res != NULL);
    assert(strcmp(cwist_http_header_get(res->headers, "grpc-status"), "0") == 0);
    {
        cwist_grpc_message meta_msg;
        assert(cwist_grpc_decode_message(res->body->data, res->body->size, &meta_msg) == 0);
        cwist_pb_reader meta_reader;
        cwist_pb_reader_init(&meta_reader, meta_msg.data, meta_msg.len);
        cwist_pb_field meta_field;
        assert(cwist_pb_read_field(&meta_reader, &meta_field) > 0);
        assert(meta_field.len == strlen("hello-meta"));
        assert(memcmp(meta_field.bytes, "hello-meta", meta_field.len) == 0);
    }
    cwist_http_response_destroy(res);
    opts.headers = NULL;
    opts.header_count = 0;

    /* gzip-compressed request message round trip. */
    {
        cwist_pb_writer zip_pb;
        cwist_pb_writer_init(&zip_pb);
        assert(cwist_pb_write_string_field(&zip_pb, 1, "ping") == 0);
        assert(cwist_pb_write_bool_field(&zip_pb, 2, 1) == 0);
        uint8_t *zipped = NULL;
        size_t zipped_len = 0;
        gzip_compress(zip_pb.data, zip_pb.len, &zipped, &zipped_len);
        uint8_t *zframe = NULL;
        size_t zframe_len = 0;
        assert(cwist_grpc_encode_message(zipped, zipped_len, 1, &zframe, &zframe_len) == 0);

        static const cwist_test_client_kv gzip_headers[] = {
            { "grpc-encoding", "gzip" },
        };
        opts.body = (const char *)zframe;
        opts.body_len = zframe_len;
        opts.headers = gzip_headers;
        opts.header_count = 1;
        res = cwist_test_client_request_ex(client, CWIST_HTTP_POST,
                                           "/cwist.test.Echo/Say", &opts);
        assert(res != NULL);
        assert(strcmp(cwist_http_header_get(res->headers, "grpc-accept-encoding"), "gzip, identity") == 0);
        decode_response(res, "reply:ping");
        cwist_http_response_destroy(res);
        opts.headers = NULL;
        opts.header_count = 0;
        cwist_free(zframe);
        cwist_free(zipped);
        cwist_pb_writer_free(&zip_pb);
    }

    /* Unsupported grpc-encoding is rejected with UNIMPLEMENTED. */
    {
        static const cwist_test_client_kv bad_enc[] = {
            { "grpc-encoding", "deflate" },
        };
        opts.body = (const char *)frame;
        opts.body_len = frame_len;
        opts.headers = bad_enc;
        opts.header_count = 1;
        res = cwist_test_client_request_ex(client, CWIST_HTTP_POST,
                                           "/cwist.test.Echo/Say", &opts);
        assert(res != NULL);
        assert(strcmp(cwist_http_header_get(res->headers, "grpc-status"), "12") == 0);
        cwist_http_response_destroy(res);
        opts.headers = NULL;
        opts.header_count = 0;
    }

    /* Compressed flag without a matching grpc-encoding is rejected. */
    {
        uint8_t *cframe = NULL;
        size_t cframe_len = 0;
        assert(cwist_grpc_encode_message(request_pb.data, request_pb.len, 1,
                                         &cframe, &cframe_len) == 0);
        opts.body = (const char *)cframe;
        opts.body_len = cframe_len;
        res = cwist_test_client_request_ex(client, CWIST_HTTP_POST,
                                           "/cwist.test.Echo/Say", &opts);
        assert(res != NULL);
        assert(strcmp(cwist_http_header_get(res->headers, "grpc-status"), "12") == 0);
        cwist_http_response_destroy(res);
        cwist_free(cframe);
    }

    /* Buffered-path recv API replays every message in order. */
    {
        cwist_sstring *recv_body = cwist_sstring_create();
        assert(recv_body != NULL);
        append_grpc_string_frame(recv_body, "aaa");
        append_grpc_string_frame(recv_body, "bbb");
        append_grpc_string_frame(recv_body, "ccc");
        opts.body = recv_body->data;
        opts.body_len = recv_body->size;
        res = cwist_test_client_request_ex(client, CWIST_HTTP_POST,
                                           "/cwist.test.Echo/Recv", &opts);
        const char *expected_recv[] = { "recv-1:aaa", "recv-2:bbb", "recv-3:ccc" };
        assert_stream_response(res, expected_recv, 3);
        cwist_http_response_destroy(res);
        cwist_sstring_destroy(recv_body);
    }

    cwist_test_client_destroy(client);
    cwist_app_destroy(app);

    /* grpc-timeout parsing */
    uint64_t ms = 0;
    assert(cwist_grpc_parse_timeout("100m", &ms) == 0 && ms == 100);
    assert(cwist_grpc_parse_timeout("2S", &ms) == 0 && ms == 2000);
    assert(cwist_grpc_parse_timeout("1M", &ms) == 0 && ms == 60000);
    assert(cwist_grpc_parse_timeout("3H", &ms) == 0 && ms == 3 * 3600000);
    assert(cwist_grpc_parse_timeout("500u", &ms) == 0 && ms == 1);
    assert(cwist_grpc_parse_timeout("99999999n", &ms) == 0 && ms == 100);
    assert(cwist_grpc_parse_timeout("", &ms) != 0);
    assert(cwist_grpc_parse_timeout("10x", &ms) != 0);
    assert(cwist_grpc_parse_timeout("1x", &ms) != 0);
    assert(cwist_grpc_parse_timeout("m", &ms) != 0);
    assert(cwist_grpc_parse_timeout("123456789m", &ms) != 0);

    cwist_free(frame);
    cwist_pb_writer_free(&request_pb);

    printf("test_grpc: OK\n");
    return 0;
}
