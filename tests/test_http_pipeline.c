/** Request bodies must not include subsequent pipelined messages (#25). */
#include <cwist/net/http/http.h>
#include <cwist/core/mem/alloc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_pipeline(const char *first, const char *body) {
    const char next[] = "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t first_len = strlen(first);
    cwist_http_async_conn_t conn = { .fd = -1 };
    conn.cap = first_len + sizeof(next);
    conn.rbuf = cwist_alloc(conn.cap);
    assert(conn.rbuf);
    memcpy(conn.rbuf, first, first_len);
    memcpy(conn.rbuf + first_len, next, sizeof(next));
    conn.len = conn.cap - 1;
    cwist_http_request *req = NULL;
    cwist_http_parse_error_t err;
    assert(cwist_http_receive_request_nb(&conn, &req, &err) == CWIST_RECV_OK);
    assert(req && err == CWIST_HTTP_PARSE_OK);
    assert(req->body && req->body->size == strlen(body));
    if (*body) assert(memcmp(req->body->data, body, strlen(body)) == 0);
    assert(conn.len == sizeof(next) - 1);
    assert(memcmp(conn.rbuf, next, sizeof(next)) == 0);
    cwist_http_request_destroy(req);
    assert(cwist_http_receive_request_nb(&conn, &req, &err) == CWIST_RECV_OK);
    assert(req && strcmp(req->path->data, "/next") == 0);
    assert(req->body->size == 0 && conn.len == 0);
    cwist_http_request_destroy(req);
    cwist_free(conn.rbuf);
}

int main(void) {
    check_pipeline("GET /first HTTP/1.1\r\nHost: x\r\n\r\n", "");
    check_pipeline("POST /first HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", "");
    check_pipeline("POST /first HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nbody", "body");
    check_pipeline("POST /first HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nbody\r\n0\r\n\r\n", "body");
    puts("HTTP pipeline body isolation tests passed");
    return 0;
}
