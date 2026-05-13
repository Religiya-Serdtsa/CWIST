#include <cwist/net/http/http.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>

void test_chunked_parsing() {
    printf("Testing chunked transfer encoding...\n");

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    const char *request =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "Hello\r\n"
        "6\r\n"
        " World\r\n"
        "0\r\n"
        "\r\n";

    size_t req_len = strlen(request);
    ssize_t written = write(sv[1], request, req_len);
    assert(written == (ssize_t)req_len);
    close(sv[1]);

    char buf[4096];
    memset(buf, 0, sizeof(buf));
    size_t buf_len = 0;
    cwist_http_request *req = cwist_http_receive_request(sv[0], buf, sizeof(buf), &buf_len);
    close(sv[0]);

    assert(req != NULL);
    assert(req->body != NULL);
    assert(strcmp(req->body->data, "Hello World") == 0);
    assert(req->body->size == 11);

    cwist_http_request_destroy(req);
    printf("Passed chunked transfer encoding.\n");
}

void test_chunked_with_trailers() {
    printf("Testing chunked with trailers...\n");

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    const char *request =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4\r\n"
        "test\r\n"
        "0\r\n"
        "X-Trailer: value\r\n"
        "\r\n";

    size_t req_len = strlen(request);
    ssize_t written = write(sv[1], request, req_len);
    assert(written == (ssize_t)req_len);
    close(sv[1]);

    char buf[4096];
    memset(buf, 0, sizeof(buf));
    size_t buf_len = 0;
    cwist_http_request *req = cwist_http_receive_request(sv[0], buf, sizeof(buf), &buf_len);
    close(sv[0]);

    assert(req != NULL);
    assert(req->body != NULL);
    assert(strcmp(req->body->data, "test") == 0);
    assert(req->body->size == 4);

    cwist_http_request_destroy(req);
    printf("Passed chunked with trailers.\n");
}

int main(void) {
    test_chunked_parsing();
    test_chunked_with_trailers();
    printf("All chunked encoding tests passed.\n");
    return 0;
}
