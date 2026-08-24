#include <cwist/net/http/http.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <stdlib.h>
#include <pthread.h>

void test_methods() {
    printf("Testing HTTP methods...\n");
    assert(strcmp(cwist_http_method_to_string(CWIST_HTTP_GET), "GET") == 0);
    assert(cwist_http_string_to_method("POST") == CWIST_HTTP_POST);
    printf("Passed methods.\n");
}

void test_request_lifecycle() {
    printf("Testing Request Lifecycle...\n");
    cwist_http_request *req = cwist_http_request_create();
    assert(req != NULL);
    assert(req->method == CWIST_HTTP_GET);
    assert(strcmp(req->version->data, "HTTP/1.1") == 0);
    assert(req->keep_alive == true);

    cwist_http_header_add(&req->headers, "Content-Type", "application/json");
    cwist_http_header_add(&req->headers, "Host", "example.com");

    assert(strcmp(cwist_http_header_get(req->headers, "Host"), "example.com") == 0);
    assert(strcmp(cwist_http_header_get(req->headers, "host"), "example.com") == 0);
    assert(strcmp(cwist_http_header_get(req->headers, "Content-Type"), "application/json") == 0);
    assert(cwist_http_header_get(req->headers, "Invalid") == NULL);

    cwist_sstring_assign(req->body, "{\"key\": \"value\"}");
    assert(strcmp(req->body->data, "{\"key\": \"value\"}") == 0);

    cwist_http_request_destroy(req);
    printf("Passed Request Lifecycle.\n");
}

void test_wasm_content_type_detection() {
    printf("Testing WASM content-type detection...\n");
    const char *tmpdir = getenv("TMPDIR");
    const int wasm_suffix_len = 5; // ".wasm"
    char path[256];
    snprintf(path, sizeof(path), "%s/cwist_test_XXXXXX.wasm", tmpdir ? tmpdir : "/tmp");
    int fd = mkstemps(path, wasm_suffix_len);
    assert(fd >= 0);
    const unsigned char wasm_magic[4] = {0x00, 0x61, 0x73, 0x6d};
    assert(write(fd, wasm_magic, sizeof(wasm_magic)) == (ssize_t)sizeof(wasm_magic));
    close(fd);

    cwist_http_response *res = cwist_http_response_create();
    assert(res != NULL);
    cwist_error_t err = cwist_http_response_send_file(res, path, NULL, NULL);
    assert(err.errtype == CWIST_ERR_INT16);
    assert(err.error.err_i16 == 0);
    assert(strcmp(cwist_http_header_get(res->headers, "content-type"), "application/wasm") == 0);

    cwist_http_response_destroy(res);
    unlink(path);
    printf("Passed WASM content-type detection.\n");
}

void test_response_lifecycle() {
    printf("Testing Response Lifecycle...\n");
    cwist_http_response *res = cwist_http_response_create();
    assert(res != NULL);
    assert(res->status_code == CWIST_HTTP_OK);

    cwist_http_header_add(&res->headers, "Server", "Cwist/0.1");
    assert(strcmp(cwist_http_header_get(res->headers, "Server"), "Cwist/0.1") == 0);

    cwist_http_response_destroy(res);
    printf("Passed Response Lifecycle.\n");
}

void test_parse_request() {
    printf("Testing Request Parsing...\n");
    const char *raw = "POST /api/users HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Type: application/json\r\n\r\n{\"name\":\"test\"}";
    
    cwist_http_request *req = cwist_http_parse_request(raw);
    assert(req != NULL);
    assert(req->method == CWIST_HTTP_POST);
    assert(strcmp(req->path->data, "/api/users") == 0);
    assert(strcmp(req->version->data, "HTTP/1.1") == 0);
    
    assert(strcmp(cwist_http_header_get(req->headers, "Host"), "localhost") == 0);
    assert(strcmp(cwist_http_header_get(req->headers, "Content-Type"), "application/json") == 0);
    
    assert(strcmp(req->body->data, "{\"name\":\"test\"}") == 0);
    assert(req->keep_alive == false);
    
    cwist_http_request_destroy(req);
    printf("Passed Request Parsing.\n");
}

void test_send_response() {
    printf("Testing Response Sending...\n");
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return;
    }

    cwist_http_response *res = cwist_http_response_create();
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->status_text, "OK");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    cwist_sstring_assign(res->body, "Hello World");
    res->keep_alive = false;

    cwist_http_send_response(sv[0], res);
    
    char buffer[1024];
    ssize_t len = recv(sv[1], buffer, sizeof(buffer) - 1, 0);
    buffer[len] = '\0';
    
    // Check key parts (order of headers might vary if implementation changes, but currently it's a stack)
    assert(strstr(buffer, "HTTP/1.1 200 OK\r\n") != NULL);
    assert(strstr(buffer, "Content-Type: text/plain\r\n") != NULL);
    assert(strstr(buffer, "Connection: close\r\n") != NULL);
    assert(strstr(buffer, "\r\nHello World") != NULL);

    cwist_http_response_destroy(res);
    close(sv[0]);
    close(sv[1]);
    printf("Passed Response Sending.\n");
}

/* --- RFC 9110/9112 compliance tests -------------------------------------- */

/* Feed a raw request through cwist_http_receive_request over a socketpair and
 * assert it is rejected with the expected parse error, then verify the error
 * response blob the app layer would send before closing. */
static void expect_parse_rejection(const char *raw, cwist_http_parse_error_t want_err, int want_status) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(write(sv[1], raw, strlen(raw)) == (ssize_t)strlen(raw));

    char buf[4096];
    memset(buf, 0, sizeof(buf));
    size_t buf_len = 0;
    cwist_http_parse_error_t perr = CWIST_HTTP_PARSE_OK;
    cwist_http_request *req = cwist_http_receive_request(sv[0], buf, sizeof(buf), &buf_len, &perr);
    assert(req == NULL);
    assert(perr == want_err);

    if (want_status > 0) {
        cwist_http_send_error_response(sv[0], want_status, NULL);
        close(sv[0]);
        char rbuf[1024];
        ssize_t n = read(sv[1], rbuf, sizeof(rbuf) - 1);
        assert(n > 0);
        rbuf[n] = '\0';
        char status_line[64];
        snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d ", want_status);
        assert(strstr(rbuf, status_line) != NULL);
        assert(strstr(rbuf, "Connection: close\r\n") != NULL);
    } else {
        close(sv[0]);
    }
    close(sv[1]);
}

/* Assert a raw request parses successfully; returns the request. */
static cwist_http_request *expect_parse_success(const char *raw) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(write(sv[1], raw, strlen(raw)) == (ssize_t)strlen(raw));
    shutdown(sv[1], SHUT_WR);

    char buf[4096];
    memset(buf, 0, sizeof(buf));
    size_t buf_len = 0;
    cwist_http_parse_error_t perr = CWIST_HTTP_PARSE_OK;
    cwist_http_request *req = cwist_http_receive_request(sv[0], buf, sizeof(buf), &buf_len, &perr);
    assert(req != NULL);
    assert(perr == CWIST_HTTP_PARSE_OK);
    close(sv[0]);
    close(sv[1]);
    return req;
}

void test_host_header_rules() {
    printf("Testing Host header rules (RFC 9112 3.2)...\n");

    /* HTTP/1.1 without Host -> 400 */
    expect_parse_rejection("GET / HTTP/1.1\r\nConnection: close\r\n\r\n",
                           CWIST_HTTP_PARSE_MALFORMED, 400);
    /* Duplicate Host -> 400 */
    expect_parse_rejection("GET / HTTP/1.1\r\nHost: a.example\r\nHost: b.example\r\n\r\n",
                           CWIST_HTTP_PARSE_MALFORMED, 400);
    /* Empty Host value -> 400 */
    expect_parse_rejection("GET / HTTP/1.1\r\nHost:\r\n\r\n",
                           CWIST_HTTP_PARSE_MALFORMED, 400);
    /* HTTP/1.0 without Host -> accepted */
    cwist_http_request *req = expect_parse_success("GET / HTTP/1.0\r\n\r\n");
    assert(strcmp(req->version->data, "HTTP/1.0") == 0);
    assert(req->keep_alive == false);
    cwist_http_request_destroy(req);

    printf("Passed Host header rules.\n");
}

void test_cl_te_smuggling_rules() {
    printf("Testing CL/TE interaction rules (RFC 9112 6.1/6.3)...\n");

    /* TE + CL together -> 400 */
    expect_parse_rejection("POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n",
                           CWIST_HTTP_PARSE_MALFORMED, 400);
    /* Duplicate CL with mismatching values -> 400 */
    expect_parse_rejection("POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\nContent-Length: 5\r\n\r\n",
                           CWIST_HTTP_PARSE_MALFORMED, 400);
    /* Non-numeric CL -> 400 */
    expect_parse_rejection("POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 4x\r\n\r\n",
                           CWIST_HTTP_PARSE_MALFORMED, 400);
    /* TE whose final coding is not chunked -> 400 */
    expect_parse_rejection("POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked, gzip\r\n\r\n",
                           CWIST_HTTP_PARSE_MALFORMED, 400);
    /* Unsupported transfer coding alongside a final chunked -> 501 */
    expect_parse_rejection("POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
                           CWIST_HTTP_PARSE_TE_UNSUPPORTED, 501);
    /* Unsupported Expect value -> 417 */
    expect_parse_rejection("POST /x HTTP/1.1\r\nHost: h\r\nExpect: bananas\r\nContent-Length: 1\r\n\r\n",
                           CWIST_HTTP_PARSE_EXPECT_FAILED, 417);

    /* Duplicate CL with identical values -> idempotent accept */
    cwist_http_request *req = expect_parse_success(
        "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\nContent-Length: 4\r\n\r\nbody");
    assert(req->content_length == 4);
    assert(strcmp(req->body->data, "body") == 0);
    cwist_http_request_destroy(req);

    printf("Passed CL/TE interaction rules.\n");
}

void test_malformed_request_line() {
    printf("Testing malformed request-line -> 400...\n");
    expect_parse_rejection("GARBAGE-NO-SPACES\r\nHost: h\r\n\r\n",
                           CWIST_HTTP_PARSE_MALFORMED, 400);
    expect_parse_rejection("GET /missing-version\r\nHost: h\r\n\r\n",
                           CWIST_HTTP_PARSE_MALFORMED, 400);
    printf("Passed malformed request-line.\n");
}

typedef struct {
    int fd;
    char buf[4096];
    size_t buf_len;
    cwist_http_request *req;
    cwist_http_parse_error_t err;
} expect_recv_ctx;

static void *expect_recv_thread(void *arg) {
    expect_recv_ctx *c = (expect_recv_ctx *)arg;
    c->req = cwist_http_receive_request(c->fd, c->buf, sizeof(c->buf), &c->buf_len, &c->err);
    return NULL;
}

void test_expect_100_continue_flow() {
    printf("Testing Expect: 100-continue flow (RFC 9110 10.1.1)...\n");

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    expect_recv_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fd = sv[0];
    ctx.err = CWIST_HTTP_PARSE_OK;

    pthread_t tid;
    assert(pthread_create(&tid, NULL, expect_recv_thread, &ctx) == 0);

    const char *headers =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Expect: 100-continue\r\n"
        "Content-Length: 5\r\n"
        "\r\n";
    assert(write(sv[1], headers, strlen(headers)) == (ssize_t)strlen(headers));

    /* The server must emit the interim 100 Continue before the body arrives. */
    char rbuf[256];
    ssize_t n = read(sv[1], rbuf, sizeof(rbuf) - 1);
    assert(n > 0);
    rbuf[n] = '\0';
    assert(strstr(rbuf, "HTTP/1.1 100 Continue\r\n\r\n") != NULL);

    assert(write(sv[1], "hello", 5) == 5);
    assert(pthread_join(tid, NULL) == 0);

    assert(ctx.req != NULL);
    assert(ctx.err == CWIST_HTTP_PARSE_OK);
    assert(ctx.req->body && strcmp(ctx.req->body->data, "hello") == 0);
    cwist_http_request_destroy(ctx.req);

    close(sv[0]);
    close(sv[1]);
    printf("Passed Expect: 100-continue flow.\n");
}

void test_head_response_headers_only() {
    printf("Testing HEAD response suppression (RFC 9110 9.3.2)...\n");

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    cwist_http_response *res = cwist_http_response_create();
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->status_text, "OK");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    cwist_sstring_assign(res->body, "Hello World");

    cwist_error_t err = cwist_http_send_response_head(sv[0], res);
    assert(err.error.err_i16 == 0);
    close(sv[0]);

    char buffer[1024];
    ssize_t len = read(sv[1], buffer, sizeof(buffer) - 1);
    assert(len > 0);
    buffer[len] = '\0';

    /* Status line and headers survive; Content-Length reflects the body... */
    assert(strstr(buffer, "HTTP/1.1 200 OK\r\n") != NULL);
    assert(strstr(buffer, "Content-Type: text/plain\r\n") != NULL);
    assert(strstr(buffer, "Content-Length: 11\r\n") != NULL);
    /* ...but no body bytes are on the wire. */
    assert(strstr(buffer, "Hello World") == NULL);
    char *body_start = strstr(buffer, "\r\n\r\n");
    assert(body_start != NULL);
    assert(body_start[4] == '\0');

    cwist_http_response_destroy(res);
    close(sv[1]);
    printf("Passed HEAD response suppression.\n");
}

int main() {
    test_methods();
    test_request_lifecycle();
    test_response_lifecycle();
    test_parse_request();
    test_send_response();
    test_wasm_content_type_detection();
    test_host_header_rules();
    test_cl_te_smuggling_rules();
    test_malformed_request_line();
    test_expect_100_continue_flow();
    test_head_response_headers_only();
    printf("All HTTP tests passed!\n");
    return 0;
}
