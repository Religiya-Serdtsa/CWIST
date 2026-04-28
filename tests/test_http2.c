#include <cwist/net/http/http2.h>
#include <cwist/net/http/https.h>
#include <cwist/core/sstring/sstring.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TEST_CERT "example/othello-web/server.crt"
#define TEST_KEY "example/othello-web/server.key"

typedef struct test_http2_server_ctx {
    int fd;
    cwist_error_t result;
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

static void http2_test_handler(void *user_ctx, cwist_http_request *req, cwist_http_response *res) {
    (void)user_ctx;
    assert(req != NULL);
    assert(strcmp(req->version->data, "HTTP/2") == 0);
    if (strcmp(req->path->data, "/") == 0) {
        cwist_http_header_add(&res->headers, "content-type", "text/plain");
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
        .negotiated_http2 = true
    };
    ctx->result = cwist_http2_serve_connection(&conn, NULL, http2_test_handler);

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ssl_ctx);
    close(ctx->fd);
    return NULL;
}

static void *http2_big_server_thread(void *arg) {
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
        .negotiated_http2 = true
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
        unsigned char payload[256] = {0};
        assert(len < sizeof(payload));
        if (len > 0) {
            assert(ssl_read_exact(client, payload, len) == 0);
        }
        if (type == 0x0) {
            if (stream_id == 1) {
                memcpy(data_buf1, payload, len);
                data_buf1[len] = '\0';
                saw_stream1 = true;
            } else if (stream_id == 3) {
                memcpy(data_buf3, payload, len);
                data_buf3[len] = '\0';
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
            if (first_data_stream == 0) first_data_stream = (int)stream_id;
            else if (second_data_stream == 0 && (int)stream_id != first_data_stream) second_data_stream = (int)stream_id;
            if (stream_id == 1) body1 += len;
            if (stream_id == 3) body3 += len;
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

int main(void) {
    test_http2_roundtrip();
    test_http2_large_body_interleave();
    printf("All HTTP/2 tests passed!\n");
    return 0;
}
