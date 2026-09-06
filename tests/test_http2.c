#include <cwist/net/http/http2.h>
#include <cwist/net/http/https.h>
#include <cwist/net/http/async.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/seq/seq.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define TEST_CERT "example/othello-web/server.crt"
#define TEST_KEY "example/othello-web/server.key"

typedef struct test_http2_server_ctx {
    int fd;
    cwist_error_t result;
    cwist_http2_request_handler_func handler;
} test_http2_server_ctx;

static int ssl_read_exact(SSL *ssl, void *buf, size_t len) {
    unsigned char *p = buf;
    size_t off = 0;
    while (off < len) {
        int n = SSL_read(ssl, p + off, (int)(len - off));
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int ssl_write_all(SSL *ssl, const void *buf, size_t len) {
    const unsigned char *p = buf;
    size_t off = 0;
    while (off < len) {
        int n = SSL_write(ssl, p + off, (int)(len - off));
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/**
 * @brief Extract the body payload from a CWIST-sequenced HTTP/2 DATA frame.
 *
 * The server now wraps every DATA frame payload in an 8-byte sequence chunk.
 * Tests that inspect body content use this helper instead of reading the raw
 * frame bytes.
 */
static bool http2_read_seq_payload(const unsigned char *payload,
                                   uint32_t len,
                                   char *out_buf,
                                   size_t out_cap,
                                   size_t *out_len) {
    cwist_seq_chunk_t chunk;
    if (!cwist_seq_chunk_parse(payload, len, &chunk)) return false;
    if (chunk.payload_len >= out_cap) return false;
    memcpy(out_buf, chunk.payload, chunk.payload_len);
    out_buf[chunk.payload_len] = '\0';
    *out_len = chunk.payload_len;
    return true;
}

static void http2_test_handler(void *user_ctx, cwist_http_request *req, cwist_http_response *res) {
    (void)user_ctx;
    assert(req != NULL);
    assert(strcmp(req->version->data, "HTTP/2") == 0);
    if (strcmp(req->path->data, "/") == 0) {
        cwist_http_header_add(&res->headers, "content-type", "text/plain");
        cwist_http_header_add(&res->headers, "x-custom", "test");
        cwist_sstring_assign(res->body, "h2 ok");
        return;
    }
    if (strcmp(req->path->data, "/two") == 0) {
        cwist_http_header_add(&res->headers, "content-type", "text/plain");
        cwist_sstring_assign(res->body, "h2 two");
        return;
    }
    cwist_http_header_add(&res->headers, "content-type", "text/plain");
    cwist_sstring_assign(res->body, "unexpected");
}

static void http2_big_handler(void *user_ctx, cwist_http_request *req, cwist_http_response *res) {
    (void)user_ctx;
    size_t body_len = 40000;
    char fill = strcmp(req->path->data, "/big1") == 0 ? 'A' : 'B';
    char *buf = malloc(body_len + 1);
    assert(buf != NULL);
    memset(buf, fill, body_len);
    buf[body_len] = '\0';
    cwist_http_header_add(&res->headers, "content-type", "text/plain");
    cwist_sstring_assign_len(res->body, buf, body_len);
    free(buf);
}

static void *http2_server_thread(void *arg) {
    test_http2_server_ctx *ctx = arg;
    SSL_CTX *ssl_ctx = SSL_CTX_new(TLS_server_method());
    assert(ssl_ctx != NULL);
    assert(SSL_CTX_use_certificate_file(ssl_ctx, TEST_CERT, SSL_FILETYPE_PEM) == 1);
    assert(SSL_CTX_use_PrivateKey_file(ssl_ctx, TEST_KEY, SSL_FILETYPE_PEM) == 1);

    SSL *ssl = SSL_new(ssl_ctx);
    assert(ssl != NULL);
    assert(SSL_set_fd(ssl, ctx->fd) == 1);
    assert(SSL_accept(ssl) == 1);

    cwist_https_connection conn = {
        .fd = ctx->fd,
        .ssl = ssl,
        .read_buf = NULL,
        .buf_len = 0,
        .negotiated_http2 = true,
        .negotiated_protocol = CWIST_HTTPS_PROTOCOL_HTTP2,
        .http2_sequenced_data = true
    };
    ctx->result = cwist_http2_serve_connection(&conn, NULL, http2_test_handler);

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ssl_ctx);
    close(ctx->fd);
    return NULL;
}

static void *http2_handler_server_thread(void *arg) {
    test_http2_server_ctx *ctx = arg;
    assert(ctx->handler != NULL);
    SSL_CTX *ssl_ctx = SSL_CTX_new(TLS_server_method());
    assert(ssl_ctx != NULL);
    assert(SSL_CTX_use_certificate_file(ssl_ctx, TEST_CERT, SSL_FILETYPE_PEM) == 1);
    assert(SSL_CTX_use_PrivateKey_file(ssl_ctx, TEST_KEY, SSL_FILETYPE_PEM) == 1);

    SSL *ssl = SSL_new(ssl_ctx);
    assert(ssl != NULL);
    assert(SSL_set_fd(ssl, ctx->fd) == 1);
    assert(SSL_accept(ssl) == 1);

    cwist_https_connection conn = {
        .fd = ctx->fd,
        .ssl = ssl,
        .read_buf = NULL,
        .buf_len = 0,
        .negotiated_http2 = true,
        .negotiated_protocol = CWIST_HTTPS_PROTOCOL_HTTP2,
        .http2_sequenced_data = true
    };
    ctx->result = cwist_http2_serve_connection(&conn, NULL, ctx->handler);

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ssl_ctx);
    close(ctx->fd);
    return NULL;
}

static void *http2_big_server_thread(void *arg) {    test_http2_server_ctx *ctx = arg;
    SSL_CTX *ssl_ctx = SSL_CTX_new(TLS_server_method());
    assert(ssl_ctx != NULL);
    assert(SSL_CTX_use_certificate_file(ssl_ctx, TEST_CERT, SSL_FILETYPE_PEM) == 1);
    assert(SSL_CTX_use_PrivateKey_file(ssl_ctx, TEST_KEY, SSL_FILETYPE_PEM) == 1);

    SSL *ssl = SSL_new(ssl_ctx);
    assert(ssl != NULL);
    assert(SSL_set_fd(ssl, ctx->fd) == 1);
    assert(SSL_accept(ssl) == 1);

    cwist_https_connection conn = {
        .fd = ctx->fd,
        .ssl = ssl,
        .read_buf = NULL,
        .buf_len = 0,
        .negotiated_http2 = true,
        .negotiated_protocol = CWIST_HTTPS_PROTOCOL_HTTP2,
        .http2_sequenced_data = true
    };
    ctx->result = cwist_http2_serve_connection(&conn, NULL, http2_big_handler);

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ssl_ctx);
    close(ctx->fd);
    return NULL;
}

static void test_http2_roundtrip(void) {
    printf("Testing HTTP/2 roundtrip...\n");
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    test_http2_server_ctx server_ctx = {
        .fd = sv[0],
        .result = make_error(CWIST_ERR_INT16)
    };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, http2_server_thread, &server_ctx) == 0);

    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, sv[1]) == 1);
    assert(SSL_connect(client) == 1);

    static const unsigned char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const unsigned char settings_frame[] = {
        0x00, 0x00, 0x00,
        0x04,
        0x00,
        0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char headers_frame1[] = {
        0x00, 0x00, 0x03,
        0x01,
        0x05,
        0x00, 0x00, 0x00, 0x01,
        0x82, 0x87, 0x84
    };
    static const unsigned char headers_frame2[] = {
        0x00, 0x00, 0x08,
        0x01,
        0x05,
        0x00, 0x00, 0x00, 0x03,
        0x82, 0x87, 0x04, 0x04, '/', 't', 'w', 'o'
    };

    assert(ssl_write_all(client, preface, sizeof(preface) - 1) == 0);
    assert(ssl_write_all(client, settings_frame, sizeof(settings_frame)) == 0);
    assert(ssl_write_all(client, headers_frame1, sizeof(headers_frame1)) == 0);
    assert(ssl_write_all(client, headers_frame2, sizeof(headers_frame2)) == 0);

    bool saw_stream1 = false;
    bool saw_stream3 = false;
    char data_buf1[64] = {0};
    char data_buf3[64] = {0};
    for (int i = 0; i < 10; ++i) {
        unsigned char hdr[9];
        assert(ssl_read_exact(client, hdr, sizeof(hdr)) == 0);
        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | (uint32_t)hdr[2];
        uint8_t type = hdr[3];
        uint32_t stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) | ((uint32_t)hdr[6] << 16) |
                             ((uint32_t)hdr[7] << 8) | hdr[8];
        unsigned char payload[4096] = {0};
        assert(len < sizeof(payload));
        if (len > 0) {
            assert(ssl_read_exact(client, payload, len) == 0);
        }
        if (type == 0x0) {
            size_t body_len = 0;
            if (stream_id == 1) {
                assert(http2_read_seq_payload(payload, len, data_buf1, sizeof(data_buf1), &body_len));
                saw_stream1 = true;
            } else if (stream_id == 3) {
                assert(http2_read_seq_payload(payload, len, data_buf3, sizeof(data_buf3), &body_len));
                saw_stream3 = true;
            }
        }
        if (saw_stream1 && saw_stream3) {
            break;
        }
    }

    assert(saw_stream1);
    assert(saw_stream3);
    assert(strcmp(data_buf1, "h2 ok") == 0);
    assert(strcmp(data_buf3, "h2 two") == 0);

    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(sv[1]);

    pthread_join(tid, NULL);
    assert(server_ctx.result.errtype == CWIST_ERR_INT16);
    assert(server_ctx.result.error.err_i16 == 0);
    printf("Passed HTTP/2 roundtrip.\n");
}

static void test_http2_large_body_interleave(void) {
    printf("Testing HTTP/2 large-body interleave...\n");
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    test_http2_server_ctx server_ctx = {
        .fd = sv[0],
        .result = make_error(CWIST_ERR_INT16)
    };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, http2_big_server_thread, &server_ctx) == 0);

    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, sv[1]) == 1);
    assert(SSL_connect(client) == 1);

    static const unsigned char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const unsigned char settings_frame[] = {
        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char headers_big1[] = {
        0x00, 0x00, 0x09, 0x01, 0x05, 0x00, 0x00, 0x00, 0x01,
        0x82, 0x87, 0x04, 0x05, '/', 'b', 'i', 'g', '1'
    };
    static const unsigned char headers_big2[] = {
        0x00, 0x00, 0x09, 0x01, 0x05, 0x00, 0x00, 0x00, 0x03,
        0x82, 0x87, 0x04, 0x05, '/', 'b', 'i', 'g', '2'
    };

    assert(ssl_write_all(client, preface, sizeof(preface) - 1) == 0);
    assert(ssl_write_all(client, settings_frame, sizeof(settings_frame)) == 0);
    assert(ssl_write_all(client, headers_big1, sizeof(headers_big1)) == 0);
    assert(ssl_write_all(client, headers_big2, sizeof(headers_big2)) == 0);

    int first_data_stream = 0;
    int second_data_stream = 0;
    bool saw_both = false;
    size_t body1 = 0;
    size_t body3 = 0;

    for (int i = 0; i < 64; ++i) {
        unsigned char hdr[9];
        assert(ssl_read_exact(client, hdr, sizeof(hdr)) == 0);
        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | (uint32_t)hdr[2];
        uint8_t type = hdr[3];
        uint32_t stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) | ((uint32_t)hdr[6] << 16) |
                             ((uint32_t)hdr[7] << 8) | hdr[8];
        unsigned char payload[17000];
        assert(len < sizeof(payload));
        if (len > 0) assert(ssl_read_exact(client, payload, len) == 0);

        if (type == 0x0) {
            size_t chunk_len = 0;
            char chunk_body[17000];
            if (!http2_read_seq_payload(payload, len, chunk_body, sizeof(chunk_body), &chunk_len)) {
                /* Should not happen with the sequenced DATA extension. */
                assert(false);
            }
            if (first_data_stream == 0) first_data_stream = (int)stream_id;
            else if (second_data_stream == 0 && (int)stream_id != first_data_stream) second_data_stream = (int)stream_id;
            if (stream_id == 1) body1 += chunk_len;
            if (stream_id == 3) body3 += chunk_len;
            if (first_data_stream != 0 && second_data_stream != 0) saw_both = true;
        }
        if (saw_both && body1 > 0 && body3 > 0) break;
    }

    assert(saw_both);
    assert(first_data_stream != second_data_stream);
    assert(body1 > 0);
    assert(body3 > 0);

    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(sv[1]);

    pthread_join(tid, NULL);
    printf("Passed HTTP/2 large-body interleave.\n");
}

static void test_http2_ping(void) {
    printf("Testing HTTP/2 PING...\n");
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    test_http2_server_ctx server_ctx = {
        .fd = sv[0],
        .result = make_error(CWIST_ERR_INT16)
    };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, http2_server_thread, &server_ctx) == 0);

    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, sv[1]) == 1);
    assert(SSL_connect(client) == 1);

    static const unsigned char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const unsigned char settings_frame[] = {
        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char ping_frame[] = {
        0x00, 0x00, 0x08,
        0x06,
        0x00,
        0x00, 0x00, 0x00, 0x00,
        0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE
    };
    static const unsigned char headers_frame[] = {
        0x00, 0x00, 0x03, 0x01, 0x05, 0x00, 0x00, 0x00, 0x01,
        0x82, 0x87, 0x84
    };

    assert(ssl_write_all(client, preface, sizeof(preface) - 1) == 0);
    assert(ssl_write_all(client, settings_frame, sizeof(settings_frame)) == 0);
    assert(ssl_write_all(client, ping_frame, sizeof(ping_frame)) == 0);
    assert(ssl_write_all(client, headers_frame, sizeof(headers_frame)) == 0);

    bool saw_ping_ack = false;
    bool saw_data = false;
    unsigned char ping_payload[8] = {0};
    for (int i = 0; i < 10; ++i) {
        unsigned char hdr[9];
        assert(ssl_read_exact(client, hdr, sizeof(hdr)) == 0);
        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | (uint32_t)hdr[2];
        uint8_t type = hdr[3];
        uint8_t flags = hdr[4];
        uint32_t stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) | ((uint32_t)hdr[6] << 16) |
                             ((uint32_t)hdr[7] << 8) | hdr[8];
        unsigned char payload[4096] = {0};
        assert(len < sizeof(payload));
        if (len > 0) assert(ssl_read_exact(client, payload, len) == 0);

        if (type == 0x06 && stream_id == 0 && (flags & 0x01)) {
            assert(len == 8);
            memcpy(ping_payload, payload, 8);
            saw_ping_ack = true;
        }
        if (type == 0x0 && stream_id == 1) {
            char data_body[64];
            size_t body_len = 0;
            assert(http2_read_seq_payload(payload, len, data_body, sizeof(data_body), &body_len));
            assert(body_len == 5);
            assert(memcmp(data_body, "h2 ok", 5) == 0);
            saw_data = true;
        }
        if (saw_ping_ack && saw_data) break;
    }

    assert(saw_ping_ack);
    assert(memcmp(ping_payload, "\xDE\xAD\xBE\xEF\xCA\xFE\xBA\xBE", 8) == 0);
    assert(saw_data);

    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(sv[1]);

    pthread_join(tid, NULL);
    printf("Passed HTTP/2 PING.\n");
}

static void test_http2_multi_stream_concurrent(void) {
    printf("Testing HTTP/2 concurrent multi-stream...\n");
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    test_http2_server_ctx server_ctx = {
        .fd = sv[0],
        .result = make_error(CWIST_ERR_INT16)
    };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, http2_server_thread, &server_ctx) == 0);

    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, sv[1]) == 1);
    assert(SSL_connect(client) == 1);

    static const unsigned char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const unsigned char settings_frame[] = {
        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    /* Stream 1: HEADERS with END_HEADERS but NOT END_STREAM */
    static const unsigned char headers_s1[] = {
        0x00, 0x00, 0x03, 0x01, 0x04,
        0x00, 0x00, 0x00, 0x01,
        0x82, 0x87, 0x84
    };
    /* Stream 3: HEADERS + END_STREAM */
    static const unsigned char headers_s3[] = {
        0x00, 0x00, 0x08, 0x01, 0x05,
        0x00, 0x00, 0x00, 0x03,
        0x82, 0x87, 0x04, 0x04, '/', 't', 'w', 'o'
    };
    /* Stream 1: empty DATA with END_STREAM */
    static const unsigned char data_s1[] = {
        0x00, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x01
    };

    assert(ssl_write_all(client, preface, sizeof(preface) - 1) == 0);
    assert(ssl_write_all(client, settings_frame, sizeof(settings_frame)) == 0);
    assert(ssl_write_all(client, headers_s1, sizeof(headers_s1)) == 0);
    assert(ssl_write_all(client, headers_s3, sizeof(headers_s3)) == 0);
    assert(ssl_write_all(client, data_s1, sizeof(data_s1)) == 0);

    bool saw_stream1 = false;
    bool saw_stream3 = false;
    for (int i = 0; i < 10; ++i) {
        unsigned char hdr[9];
        assert(ssl_read_exact(client, hdr, sizeof(hdr)) == 0);
        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | (uint32_t)hdr[2];
        uint8_t type = hdr[3];
        uint32_t stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) | ((uint32_t)hdr[6] << 16) |
                             ((uint32_t)hdr[7] << 8) | hdr[8];
        unsigned char payload[4096] = {0};
        assert(len < sizeof(payload));
        if (len > 0) assert(ssl_read_exact(client, payload, len) == 0);

        if (type == 0x0 && stream_id == 1) {
            char data_body[64];
            size_t body_len = 0;
            assert(http2_read_seq_payload(payload, len, data_body, sizeof(data_body), &body_len));
            assert(body_len == 5);
            assert(memcmp(data_body, "h2 ok", 5) == 0);
            saw_stream1 = true;
        }
        if (type == 0x0 && stream_id == 3) {
            char data_body[64];
            size_t body_len = 0;
            assert(http2_read_seq_payload(payload, len, data_body, sizeof(data_body), &body_len));
            assert(body_len == 6);
            assert(memcmp(data_body, "h2 two", 6) == 0);
            saw_stream3 = true;
        }
        if (saw_stream1 && saw_stream3) break;
    }

    assert(saw_stream1);
    assert(saw_stream3);

    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(sv[1]);

    pthread_join(tid, NULL);
    printf("Passed HTTP/2 concurrent multi-stream.\n");
}

static void test_http2_continuation(void) {
    printf("Testing HTTP/2 CONTINUATION...\n");
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    test_http2_server_ctx server_ctx = {
        .fd = sv[0],
        .result = make_error(CWIST_ERR_INT16)
    };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, http2_server_thread, &server_ctx) == 0);

    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, sv[1]) == 1);
    assert(SSL_connect(client) == 1);

    static const unsigned char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const unsigned char settings_frame[] = {
        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    /* HEADERS with END_STREAM but without END_HEADERS: only :method GET (0x82) */
    static const unsigned char headers_partial[] = {
        0x00, 0x00, 0x01, 0x01, 0x01,
        0x00, 0x00, 0x00, 0x01,
        0x82
    };
    /* CONTINUATION with END_HEADERS: :scheme https (0x87), :path / (0x84) */
    static const unsigned char continuation_frame[] = {
        0x00, 0x00, 0x02, 0x09, 0x04,
        0x00, 0x00, 0x00, 0x01,
        0x87, 0x84
    };

    assert(ssl_write_all(client, preface, sizeof(preface) - 1) == 0);
    assert(ssl_write_all(client, settings_frame, sizeof(settings_frame)) == 0);
    assert(ssl_write_all(client, headers_partial, sizeof(headers_partial)) == 0);
    assert(ssl_write_all(client, continuation_frame, sizeof(continuation_frame)) == 0);

    bool saw_data = false;
    for (int i = 0; i < 10; ++i) {
        unsigned char hdr[9];
        assert(ssl_read_exact(client, hdr, sizeof(hdr)) == 0);
        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | (uint32_t)hdr[2];
        uint8_t type = hdr[3];
        uint32_t stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) | ((uint32_t)hdr[6] << 16) |
                             ((uint32_t)hdr[7] << 8) | hdr[8];
        unsigned char payload[4096] = {0};
        assert(len < sizeof(payload));
        if (len > 0) assert(ssl_read_exact(client, payload, len) == 0);

        if (type == 0x0 && stream_id == 1) {
            char data_body[64];
            size_t body_len = 0;
            assert(http2_read_seq_payload(payload, len, data_body, sizeof(data_body), &body_len));
            assert(body_len == 5);
            assert(memcmp(data_body, "h2 ok", 5) == 0);
            saw_data = true;
            break;
        }
    }

    assert(saw_data);

    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(sv[1]);

    pthread_join(tid, NULL);
    printf("Passed HTTP/2 CONTINUATION.\n");
}

static void test_http2_flow_control(void) {
    printf("Testing HTTP/2 flow control...\n");
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    test_http2_server_ctx server_ctx = {
        .fd = sv[0],
        .result = make_error(CWIST_ERR_INT16)
    };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, http2_big_server_thread, &server_ctx) == 0);

    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, sv[1]) == 1);
    assert(SSL_connect(client) == 1);

    static const unsigned char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    /* SETTINGS: INITIAL_WINDOW_SIZE = 100 */
    static const unsigned char settings_frame[] = {
        0x00, 0x00, 0x06, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x04, 0x00, 0x00, 0x00, 0x64
    };
    static const unsigned char headers_big1[] = {
        0x00, 0x00, 0x09, 0x01, 0x05, 0x00, 0x00, 0x00, 0x01,
        0x82, 0x87, 0x04, 0x05, '/', 'b', 'i', 'g', '1'
    };

    assert(ssl_write_all(client, preface, sizeof(preface) - 1) == 0);
    assert(ssl_write_all(client, settings_frame, sizeof(settings_frame)) == 0);

    /* Read frames until we see a SETTINGS ACK from the server */
    bool saw_settings_ack = false;
    while (!saw_settings_ack) {
        unsigned char fh[9];
        assert(ssl_read_exact(client, fh, sizeof(fh)) == 0);
        uint32_t flen = ((uint32_t)fh[0] << 16) | ((uint32_t)fh[1] << 8) | (uint32_t)fh[2];
        uint8_t ftype = fh[3];
        uint8_t fflags = fh[4];
        if (flen > 0) {
            unsigned char *discard = (unsigned char *)malloc(flen);
            assert(discard);
            assert(ssl_read_exact(client, discard, flen) == 0);
            free(discard);
        }
        if (ftype == 0x04 && (fflags & 0x01)) {
            saw_settings_ack = true;
        } else if (ftype == 0x04) {
            /* Server sent non-ACK SETTINGS; acknowledge it */
            static const unsigned char ack[] = {
                0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00
            };
            assert(ssl_write_all(client, ack, sizeof(ack)) == 0);
        }
    }
    /* ACK any remaining server SETTINGS */
    static const unsigned char settings_ack[] = {
        0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00
    };
    assert(ssl_write_all(client, settings_ack, sizeof(settings_ack)) == 0);

    assert(ssl_write_all(client, headers_big1, sizeof(headers_big1)) == 0);

    size_t total_body = 0;
    bool got_end_stream = false;
    for (int i = 0; i < 128 && !got_end_stream; ++i) {
        unsigned char hdr[9];
        if (ssl_read_exact(client, hdr, sizeof(hdr)) != 0) break;
        uint32_t flen = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | (uint32_t)hdr[2];
        uint8_t type = hdr[3];
        uint8_t flags = hdr[4];
        uint32_t stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) | ((uint32_t)hdr[6] << 16) |
                             ((uint32_t)hdr[7] << 8) | hdr[8];
        unsigned char payload[17000];
        assert(flen < sizeof(payload));
        if (flen > 0) assert(ssl_read_exact(client, payload, flen) == 0);

        if (type == 0x0 && stream_id == 1) {
            char chunk_body[17000];
            size_t chunk_len = 0;
            if (http2_read_seq_payload(payload, flen, chunk_body, sizeof(chunk_body), &chunk_len)) {
                total_body += chunk_len;
            }
            if (flags & 0x01) got_end_stream = true;
            if (total_body > 0) break;
        }
    }

    /* Server should only send ~100 bytes before flow control blocks it */
    assert(total_body <= 100);
    assert(!got_end_stream);

    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(sv[1]);

    pthread_join(tid, NULL);
    printf("Passed HTTP/2 flow control.\n");
}

static void test_http2_response_headers(void) {
    printf("Testing HTTP/2 response headers...\n");
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    test_http2_server_ctx server_ctx = {
        .fd = sv[0],
        .result = make_error(CWIST_ERR_INT16)
    };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, http2_server_thread, &server_ctx) == 0);

    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, sv[1]) == 1);
    assert(SSL_connect(client) == 1);

    static const unsigned char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const unsigned char settings_frame[] = {
        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char headers_frame[] = {
        0x00, 0x00, 0x03, 0x01, 0x05, 0x00, 0x00, 0x00, 0x01,
        0x82, 0x87, 0x84
    };

    assert(ssl_write_all(client, preface, sizeof(preface) - 1) == 0);
    assert(ssl_write_all(client, settings_frame, sizeof(settings_frame)) == 0);
    assert(ssl_write_all(client, headers_frame, sizeof(headers_frame)) == 0);

    bool saw_headers = false;
    bool saw_data = false;
    for (int i = 0; i < 10; ++i) {
        unsigned char hdr[9];
        assert(ssl_read_exact(client, hdr, sizeof(hdr)) == 0);
        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | (uint32_t)hdr[2];
        uint8_t type = hdr[3];
        uint32_t stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) | ((uint32_t)hdr[6] << 16) |
                             ((uint32_t)hdr[7] << 8) | hdr[8];
        unsigned char payload[4096] = {0};
        assert(len < sizeof(payload));
        if (len > 0) assert(ssl_read_exact(client, payload, len) == 0);

        if (type == 0x1 && stream_id == 1) {
            /* HEADERS frame should contain :status, content-length,
             * content-type, and x-custom. Old code sent only 1 byte (0x88). */
            assert(len > 1);
            saw_headers = true;
        }
        if (type == 0x0 && stream_id == 1) {
            char data_body[64];
            size_t body_len = 0;
            assert(http2_read_seq_payload(payload, len, data_body, sizeof(data_body), &body_len));
            assert(body_len == 5);
            assert(memcmp(data_body, "h2 ok", 5) == 0);
            saw_data = true;
        }
        if (saw_headers && saw_data) break;
    }

    assert(saw_headers);
    assert(saw_data);

    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(sv[1]);

    pthread_join(tid, NULL);
    printf("Passed HTTP/2 response headers.\n");
}

/* Handler for the HPACK incremental-indexing test: replies "h2 ok" only when
 * the request carried x-token: abc123 (delivered via the dynamic table on the
 * second request). */
static void http2_hpack_handler(void *user_ctx, cwist_http_request *req, cwist_http_response *res) {
    (void)user_ctx;
    char *token = cwist_http_header_get(req->headers, "x-token");
    cwist_http_header_add(&res->headers, "content-type", "text/plain");
    if (token && strcmp(token, "abc123") == 0) {
        cwist_sstring_assign(res->body, "h2 ok");
    } else {
        cwist_sstring_assign(res->body, "unexpected");
    }
}

static void test_http2_hpack_incremental_indexing(void) {
    printf("Testing HTTP/2 HPACK incremental indexing...\n");
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    test_http2_server_ctx server_ctx = {
        .fd = sv[0],
        .result = make_error(CWIST_ERR_INT16),
        .handler = http2_hpack_handler
    };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, http2_handler_server_thread, &server_ctx) == 0);

    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, sv[1]) == 1);
    assert(SSL_connect(client) == 1);

    static const unsigned char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const unsigned char settings_frame[] = {
        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    /* Stream 1: :method GET, :scheme https, :path /, then literal-with-
     * incremental-indexing "x-token: abc123" (inserts dynamic index 62). */
    static const unsigned char headers_s1[] = {
        0x00, 0x00, 0x13, 0x01, 0x05,
        0x00, 0x00, 0x00, 0x01,
        0x82, 0x87, 0x84,
        0x40, 0x07, 'x', '-', 't', 'o', 'k', 'e', 'n',
        0x06, 'a', 'b', 'c', '1', '2', '3'
    };
    /* Stream 3: same pseudo-headers plus indexed field 62 (0x80|62 = 0xBE),
     * resolving to x-token: abc123 from the dynamic table. */
    static const unsigned char headers_s3[] = {
        0x00, 0x00, 0x04, 0x01, 0x05,
        0x00, 0x00, 0x00, 0x03,
        0x82, 0x87, 0x84, 0xBE
    };

    assert(ssl_write_all(client, preface, sizeof(preface) - 1) == 0);
    assert(ssl_write_all(client, settings_frame, sizeof(settings_frame)) == 0);
    assert(ssl_write_all(client, headers_s1, sizeof(headers_s1)) == 0);
    assert(ssl_write_all(client, headers_s3, sizeof(headers_s3)) == 0);

    bool saw_stream1 = false;
    bool saw_stream3 = false;
    for (int i = 0; i < 16 && !(saw_stream1 && saw_stream3); ++i) {
        unsigned char hdr[9];
        assert(ssl_read_exact(client, hdr, sizeof(hdr)) == 0);
        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | (uint32_t)hdr[2];
        uint8_t type = hdr[3];
        uint32_t stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) | ((uint32_t)hdr[6] << 16) |
                             ((uint32_t)hdr[7] << 8) | hdr[8];
        unsigned char payload[4096] = {0};
        assert(len < sizeof(payload));
        if (len > 0) assert(ssl_read_exact(client, payload, len) == 0);

        if (type == 0x0 && (stream_id == 1 || stream_id == 3)) {
            char data_body[64];
            size_t body_len = 0;
            assert(http2_read_seq_payload(payload, len, data_body, sizeof(data_body), &body_len));
            assert(strcmp(data_body, "h2 ok") == 0);
            if (stream_id == 1) saw_stream1 = true;
            else saw_stream3 = true;
        }
    }

    assert(saw_stream1);
    assert(saw_stream3);

    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(sv[1]);

    pthread_join(tid, NULL);
    printf("Passed HTTP/2 HPACK incremental indexing.\n");
}

static void test_http2_missing_pseudo_header(void) {
    printf("Testing HTTP/2 missing pseudo-header rejection...\n");
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    test_http2_server_ctx server_ctx = {
        .fd = sv[0],
        .result = make_error(CWIST_ERR_INT16),
        .handler = http2_test_handler
    };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, http2_handler_server_thread, &server_ctx) == 0);

    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, sv[1]) == 1);
    assert(SSL_connect(client) == 1);

    static const unsigned char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const unsigned char settings_frame[] = {
        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    /* :method GET + :scheme https, but no :path: RFC 7540 §8.1.2.3 violation. */
    static const unsigned char headers_bad[] = {
        0x00, 0x00, 0x02, 0x01, 0x05,
        0x00, 0x00, 0x00, 0x01,
        0x82, 0x87
    };

    assert(ssl_write_all(client, preface, sizeof(preface) - 1) == 0);
    assert(ssl_write_all(client, settings_frame, sizeof(settings_frame)) == 0);
    assert(ssl_write_all(client, headers_bad, sizeof(headers_bad)) == 0);

    bool saw_rst = false;
    for (int i = 0; i < 10 && !saw_rst; ++i) {
        unsigned char hdr[9];
        assert(ssl_read_exact(client, hdr, sizeof(hdr)) == 0);
        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | (uint32_t)hdr[2];
        uint8_t type = hdr[3];
        uint32_t stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) | ((uint32_t)hdr[6] << 16) |
                             ((uint32_t)hdr[7] << 8) | hdr[8];
        unsigned char payload[4096] = {0};
        assert(len < sizeof(payload));
        if (len > 0) assert(ssl_read_exact(client, payload, len) == 0);

        if (type == 0x3 && stream_id == 1) {
            assert(len == 4);
            uint32_t err = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                           ((uint32_t)payload[2] << 8) | (uint32_t)payload[3];
            assert(err == 0x1); /* PROTOCOL_ERROR */
            saw_rst = true;
        }
    }

    assert(saw_rst);

    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(sv[1]);

    pthread_join(tid, NULL);
    printf("Passed HTTP/2 missing pseudo-header rejection.\n");
}

static void test_http2_max_concurrent_streams(void) {
    printf("Testing HTTP/2 max concurrent streams...\n");
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    test_http2_server_ctx server_ctx = {
        .fd = sv[0],
        .result = make_error(CWIST_ERR_INT16),
        .handler = http2_test_handler
    };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, http2_handler_server_thread, &server_ctx) == 0);

    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, sv[1]) == 1);
    assert(SSL_connect(client) == 1);

    static const unsigned char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const unsigned char settings_frame[] = {
        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    /* HEADERS, END_HEADERS but no END_STREAM: stream stays open. */
    static const unsigned char headers_open[] = {
        0x00, 0x00, 0x03, 0x01, 0x04,
        0x00, 0x00, 0x00, 0x00,
        0x82, 0x87, 0x84
    };

    assert(ssl_write_all(client, preface, sizeof(preface) - 1) == 0);
    assert(ssl_write_all(client, settings_frame, sizeof(settings_frame)) == 0);

    /* Open 101 streams (ids 1..201); the advertised/compiled limit is 100,
     * so stream 201 must be refused. */
    const uint32_t limit = 100;
    const uint32_t last_id = 2 * (limit + 1) - 1;
    for (uint32_t id = 1; id <= last_id; id += 2) {
        unsigned char frame[sizeof(headers_open)];
        memcpy(frame, headers_open, sizeof(frame));
        frame[5] = (unsigned char)((id >> 24) & 0x7f);
        frame[6] = (unsigned char)((id >> 16) & 0xff);
        frame[7] = (unsigned char)((id >> 8) & 0xff);
        frame[8] = (unsigned char)(id & 0xff);
        assert(ssl_write_all(client, frame, sizeof(frame)) == 0);
    }

    bool saw_refused = false;
    for (int i = 0; i < 64 && !saw_refused; ++i) {
        unsigned char hdr[9];
        assert(ssl_read_exact(client, hdr, sizeof(hdr)) == 0);
        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | (uint32_t)hdr[2];
        uint8_t type = hdr[3];
        uint32_t stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) | ((uint32_t)hdr[6] << 16) |
                             ((uint32_t)hdr[7] << 8) | hdr[8];
        unsigned char payload[4096] = {0};
        assert(len < sizeof(payload));
        if (len > 0) assert(ssl_read_exact(client, payload, len) == 0);

        if (type == 0x3 && stream_id == last_id) {
            assert(len == 4);
            uint32_t err = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                           ((uint32_t)payload[2] << 8) | (uint32_t)payload[3];
            assert(err == 0x7); /* REFUSED_STREAM */
            saw_refused = true;
        }
    }

    assert(saw_refused);

    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(sv[1]);

    pthread_join(tid, NULL);
    printf("Passed HTTP/2 max concurrent streams.\n");
}

static void test_http2_rapid_reset(void) {
    printf("Testing HTTP/2 Rapid Reset defense (CVE-2023-44487)...\n");
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    test_http2_server_ctx server_ctx = {
        .fd = sv[0],
        .result = make_error(CWIST_ERR_INT16),
        .handler = http2_test_handler
    };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, http2_handler_server_thread, &server_ctx) == 0);

    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, sv[1]) == 1);
    assert(SSL_connect(client) == 1);

    static const unsigned char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const unsigned char settings_frame[] = {
        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    assert(ssl_write_all(client, preface, sizeof(preface) - 1) == 0);
    assert(ssl_write_all(client, settings_frame, sizeof(settings_frame)) == 0);

    /* Send rapid reset attack: 150 consecutive HEADERS + RST_STREAM pairs */
    bool saw_goaway = false;
    uint32_t goaway_err = 0;
    for (uint32_t i = 1; i <= 150; i++) {
        uint32_t stream_id = 2 * i - 1;
        /* HEADERS frame on stream_id with END_HEADERS only (no END_STREAM) */
        unsigned char hframe[12] = {
            0x00, 0x00, 0x03, 0x01, 0x04,
            (unsigned char)((stream_id >> 24) & 0x7f),
            (unsigned char)((stream_id >> 16) & 0xff),
            (unsigned char)((stream_id >> 8) & 0xff),
            (unsigned char)(stream_id & 0xff),
            0x82, 0x87, 0x84 /* :method GET, :scheme https, :path / */
        };
        /* RST_STREAM frame on stream_id with CANCEL (0x8) */
        unsigned char rst_frame[13] = {
            0x00, 0x00, 0x04, 0x03, 0x00,
            (unsigned char)((stream_id >> 24) & 0x7f),
            (unsigned char)((stream_id >> 16) & 0xff),
            (unsigned char)((stream_id >> 8) & 0xff),
            (unsigned char)(stream_id & 0xff),
            0x00, 0x00, 0x00, 0x08
        };
        if (ssl_write_all(client, hframe, sizeof(hframe)) != 0 ||
            ssl_write_all(client, rst_frame, sizeof(rst_frame)) != 0) {
            break;
        }
    }

    /* Read incoming frames to find the GOAWAY frame with ENHANCE_YOUR_CALM (0xb) */
    for (int i = 0; i < 200 && !saw_goaway; ++i) {
        unsigned char hdr[9];
        if (ssl_read_exact(client, hdr, sizeof(hdr)) != 0) break;
        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | hdr[2];
        uint8_t type = hdr[3];
        unsigned char payload[4096] = {0};
        assert(len < sizeof(payload));
        if (len > 0 && ssl_read_exact(client, payload, len) != 0) break;

        if (type == 0x07) { /* GOAWAY */
            assert(len >= 8);
            goaway_err = ((uint32_t)payload[4] << 24) | ((uint32_t)payload[5] << 16) |
                         ((uint32_t)payload[6] << 8) | (uint32_t)payload[7];
            saw_goaway = true;
        }
    }

    assert(saw_goaway);
    assert(goaway_err == 0xb); /* ENHANCE_YOUR_CALM */

    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(sv[1]);

    pthread_join(tid, NULL);
    printf("Passed HTTP/2 Rapid Reset defense (CVE-2023-44487).\n");
}

static void test_http2_idle_stream_rst(void) {
    printf("Testing HTTP/2 idle stream RST_STREAM rejection (RFC 7540 §5.1)...\n");
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    test_http2_server_ctx server_ctx = {
        .fd = sv[0],
        .result = make_error(CWIST_ERR_INT16),
        .handler = http2_test_handler
    };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, http2_handler_server_thread, &server_ctx) == 0);

    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, sv[1]) == 1);
    assert(SSL_connect(client) == 1);

    static const unsigned char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const unsigned char settings_frame[] = {
        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    assert(ssl_write_all(client, preface, sizeof(preface) - 1) == 0);
    assert(ssl_write_all(client, settings_frame, sizeof(settings_frame)) == 0);

    /* Send RST_STREAM on stream 99 (never opened, idle state) */
    uint32_t idle_stream_id = 99;
    unsigned char rst_frame[13] = {
        0x00, 0x00, 0x04, 0x03, 0x00,
        (unsigned char)((idle_stream_id >> 24) & 0x7f),
        (unsigned char)((idle_stream_id >> 16) & 0xff),
        (unsigned char)((idle_stream_id >> 8) & 0xff),
        (unsigned char)(idle_stream_id & 0xff),
        0x00, 0x00, 0x00, 0x08
    };
    assert(ssl_write_all(client, rst_frame, sizeof(rst_frame)) == 0);

    bool saw_goaway = false;
    uint32_t goaway_err = 0;
    for (int i = 0; i < 32 && !saw_goaway; ++i) {
        unsigned char hdr[9];
        if (ssl_read_exact(client, hdr, sizeof(hdr)) != 0) break;
        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | hdr[2];
        uint8_t type = hdr[3];
        unsigned char payload[4096] = {0};
        assert(len < sizeof(payload));
        if (len > 0 && ssl_read_exact(client, payload, len) != 0) break;

        if (type == 0x07) { /* GOAWAY */
            assert(len >= 8);
            goaway_err = ((uint32_t)payload[4] << 24) | ((uint32_t)payload[5] << 16) |
                         ((uint32_t)payload[6] << 8) | (uint32_t)payload[7];
            saw_goaway = true;
        }
    }

    assert(saw_goaway);
    assert(goaway_err == 0x1); /* PROTOCOL_ERROR per RFC 7540 §5.1 */

    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(sv[1]);

    pthread_join(tid, NULL);
    printf("Passed HTTP/2 idle stream RST_STREAM rejection (RFC 7540 §5.1).\n");
}

static void test_http2_continuation_flood(void) {
    printf("Testing HTTP/2 CONTINUATION flood defense (CVE-2024-27983)...\n");
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    test_http2_server_ctx server_ctx = {
        .fd = sv[0],
        .result = make_error(CWIST_ERR_INT16),
        .handler = http2_test_handler
    };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, http2_handler_server_thread, &server_ctx) == 0);

    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, sv[1]) == 1);
    assert(SSL_connect(client) == 1);

    static const unsigned char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const unsigned char settings_frame[] = {
        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    assert(ssl_write_all(client, preface, sizeof(preface) - 1) == 0);
    assert(ssl_write_all(client, settings_frame, sizeof(settings_frame)) == 0);

    /* HEADERS without END_HEADERS on stream 1 */
    static const unsigned char headers_start[] = {
        0x00, 0x00, 0x02, 0x01, 0x00, /* len=2, type=1 (HEADERS), flags=0 (no END_HEADERS) */
        0x00, 0x00, 0x00, 0x01,
        0x82, 0x87
    };
    assert(ssl_write_all(client, headers_start, sizeof(headers_start)) == 0);

    /* Send 35 empty CONTINUATION frames (limit is 32) */
    static const unsigned char continuation[] = {
        0x00, 0x00, 0x00, 0x09, 0x00, /* len=0, type=9 (CONTINUATION), flags=0 */
        0x00, 0x00, 0x00, 0x01
    };
    for (int i = 0; i < 35; i++) {
        if (ssl_write_all(client, continuation, sizeof(continuation)) != 0) break;
    }

    bool saw_goaway = false;
    uint32_t goaway_err = 0;
    for (int i = 0; i < 64 && !saw_goaway; ++i) {
        unsigned char hdr[9];
        if (ssl_read_exact(client, hdr, sizeof(hdr)) != 0) break;
        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | hdr[2];
        uint8_t type = hdr[3];
        unsigned char payload[4096] = {0};
        assert(len < sizeof(payload));
        if (len > 0 && ssl_read_exact(client, payload, len) != 0) break;

        if (type == 0x07) { /* GOAWAY */
            assert(len >= 8);
            goaway_err = ((uint32_t)payload[4] << 24) | ((uint32_t)payload[5] << 16) |
                         ((uint32_t)payload[6] << 8) | (uint32_t)payload[7];
            saw_goaway = true;
        }
    }

    assert(saw_goaway);
    assert(goaway_err == 0xb); /* ENHANCE_YOUR_CALM */

    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(sv[1]);

    pthread_join(tid, NULL);
    printf("Passed HTTP/2 CONTINUATION flood defense (CVE-2024-27983).\n");
}

/* Worker that completes a deferred exchange after a short delay. */
static void *http2_async_respond_thread(void *arg) {
    cwist_async *a = (cwist_async *)arg;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    assert(cwist_async_respond(a, CWIST_HTTP_OK, "text/plain", "slow ok", 7));
    return NULL;
}

/* Handler: /slow defers and is answered by a worker thread ~50ms later;
 * everything else is answered inline. */
static void http2_async_defer_handler(void *user_ctx, cwist_http_request *req, cwist_http_response *res) {
    (void)user_ctx;
    if (strcmp(req->path->data, "/slow") == 0) {
        cwist_async *a = cwist_async_defer(req, res);
        assert(a != NULL);
        pthread_t t;
        assert(pthread_create(&t, NULL, http2_async_respond_thread, a) == 0);
        pthread_detach(t);
        return;
    }
    cwist_http_header_add(&res->headers, "content-type", "text/plain");
    cwist_sstring_assign(res->body, "fast ok");
}

static void test_http2_async_defer(void) {
    printf("Testing HTTP/2 async defer...\n");
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    test_http2_server_ctx server_ctx = {
        .fd = sv[0],
        .result = make_error(CWIST_ERR_INT16),
        .handler = http2_async_defer_handler
    };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, http2_handler_server_thread, &server_ctx) == 0);

    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, sv[1]) == 1);
    assert(SSL_connect(client) == 1);

    static const unsigned char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const unsigned char settings_frame[] = {
        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char headers_slow[] = {
        0x00, 0x00, 0x09, 0x01, 0x05, 0x00, 0x00, 0x00, 0x01,
        0x82, 0x87, 0x04, 0x05, '/', 's', 'l', 'o', 'w'
    };
    static const unsigned char headers_fast[] = {
        0x00, 0x00, 0x09, 0x01, 0x05, 0x00, 0x00, 0x00, 0x03,
        0x82, 0x87, 0x04, 0x05, '/', 'f', 'a', 's', 't'
    };

    assert(ssl_write_all(client, preface, sizeof(preface) - 1) == 0);
    assert(ssl_write_all(client, settings_frame, sizeof(settings_frame)) == 0);
    assert(ssl_write_all(client, headers_slow, sizeof(headers_slow)) == 0);
    assert(ssl_write_all(client, headers_fast, sizeof(headers_fast)) == 0);

    /* The deferred stream 1 must not stall the connection: stream 3's
     * response has to arrive first, then stream 1's deferred response. */
    int first_data_stream = 0;
    bool saw_stream1 = false;
    bool saw_stream3 = false;
    char data_buf1[64] = {0};
    char data_buf3[64] = {0};
    for (int i = 0; i < 20; ++i) {
        unsigned char hdr[9];
        assert(ssl_read_exact(client, hdr, sizeof(hdr)) == 0);
        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | (uint32_t)hdr[2];
        uint8_t type = hdr[3];
        uint32_t stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) | ((uint32_t)hdr[6] << 16) |
                             ((uint32_t)hdr[7] << 8) | hdr[8];
        unsigned char payload[4096] = {0};
        assert(len < sizeof(payload));
        if (len > 0) assert(ssl_read_exact(client, payload, len) == 0);

        if (type == 0x0) {
            size_t body_len = 0;
            if (stream_id == 1) {
                assert(http2_read_seq_payload(payload, len, data_buf1, sizeof(data_buf1), &body_len));
                saw_stream1 = true;
            } else if (stream_id == 3) {
                assert(http2_read_seq_payload(payload, len, data_buf3, sizeof(data_buf3), &body_len));
                saw_stream3 = true;
            }
            if (first_data_stream == 0) first_data_stream = (int)stream_id;
        }
        if (saw_stream1 && saw_stream3) break;
    }

    assert(saw_stream1);
    assert(saw_stream3);
    assert(first_data_stream == 3); /* fast stream answered before the deferred one */
    assert(strcmp(data_buf1, "slow ok") == 0);
    assert(strcmp(data_buf3, "fast ok") == 0);

    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(sv[1]);

    pthread_join(tid, NULL);
    assert(server_ctx.result.errtype == CWIST_ERR_INT16);
    assert(server_ctx.result.error.err_i16 == 0);
    printf("Passed HTTP/2 async defer.\n");
}

/* RST_STREAM on a deferred stream: the late completion must be dropped
 * without sending frames or freeing the stream state twice. */
static void test_http2_async_defer_rst_drops(void) {
    printf("Testing HTTP/2 async defer RST drop...\n");
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    test_http2_server_ctx server_ctx = {
        .fd = sv[0],
        .result = make_error(CWIST_ERR_INT16),
        .handler = http2_async_defer_handler
    };
    pthread_t tid;
    assert(pthread_create(&tid, NULL, http2_handler_server_thread, &server_ctx) == 0);

    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, sv[1]) == 1);
    assert(SSL_connect(client) == 1);

    static const unsigned char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const unsigned char settings_frame[] = {
        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const unsigned char headers_slow[] = {
        0x00, 0x00, 0x09, 0x01, 0x05, 0x00, 0x00, 0x00, 0x01,
        0x82, 0x87, 0x04, 0x05, '/', 's', 'l', 'o', 'w'
    };
    static const unsigned char rst_stream1[] = {
        0x00, 0x00, 0x04, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x08 /* CANCEL */
    };
    static const unsigned char headers_fast[] = {
        0x00, 0x00, 0x09, 0x01, 0x05, 0x00, 0x00, 0x00, 0x03,
        0x82, 0x87, 0x04, 0x05, '/', 'f', 'a', 's', 't'
    };

    assert(ssl_write_all(client, preface, sizeof(preface) - 1) == 0);
    assert(ssl_write_all(client, settings_frame, sizeof(settings_frame)) == 0);
    assert(ssl_write_all(client, headers_slow, sizeof(headers_slow)) == 0);
    assert(ssl_write_all(client, rst_stream1, sizeof(rst_stream1)) == 0);
    assert(ssl_write_all(client, headers_fast, sizeof(headers_fast)) == 0);

    /* Read well past the worker's 50ms delay: no frame for stream 1 may
     * appear, the connection stays healthy, and stream 3 is answered. */
    bool saw_stream3 = false;
    char data_buf3[64] = {0};
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500 * 1000 };
    assert(setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
    for (int i = 0; i < 64; ++i) {
        unsigned char hdr[9];
        if (ssl_read_exact(client, hdr, sizeof(hdr)) != 0) break; /* idle: timeout */
        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | (uint32_t)hdr[2];
        uint8_t type = hdr[3];
        uint32_t stream_id = (((uint32_t)hdr[5] & 0x7f) << 24) | ((uint32_t)hdr[6] << 16) |
                             ((uint32_t)hdr[7] << 8) | hdr[8];
        assert(stream_id != 1); /* cancelled stream must never be answered */
        unsigned char payload[4096] = {0};
        assert(len < sizeof(payload));
        if (len > 0) assert(ssl_read_exact(client, payload, len) == 0);
        if (type == 0x0 && stream_id == 3) {
            size_t body_len = 0;
            assert(http2_read_seq_payload(payload, len, data_buf3, sizeof(data_buf3), &body_len));
            saw_stream3 = true;
        }
    }

    assert(saw_stream3);
    assert(strcmp(data_buf3, "fast ok") == 0);

    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(sv[1]);

    pthread_join(tid, NULL);
    assert(server_ctx.result.errtype == CWIST_ERR_INT16);
    assert(server_ctx.result.error.err_i16 == 0);
    printf("Passed HTTP/2 async defer RST drop.\n");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_http2_roundtrip();
    test_http2_large_body_interleave();
    test_http2_ping();
    test_http2_multi_stream_concurrent();
    test_http2_continuation();
    test_http2_flow_control();
    test_http2_response_headers();
    test_http2_hpack_incremental_indexing();
    test_http2_async_defer();
    test_http2_async_defer_rst_drops();
    test_http2_missing_pseudo_header();
    test_http2_max_concurrent_streams();
    test_http2_rapid_reset();
    test_http2_idle_stream_rst();
    test_http2_continuation_flood();
    printf("All HTTP/2 tests passed!\n");
    return 0;
}

