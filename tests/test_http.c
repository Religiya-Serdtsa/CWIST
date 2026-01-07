#include <cwist/net/http/http.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>

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
    assert(strcmp(cwist_http_header_get(req->headers, "Content-Type"), "application/json") == 0);
    assert(cwist_http_header_get(req->headers, "Invalid") == NULL);

    cwist_sstring_assign(req->body, "{\"key\": \"value\"}");
    assert(strcmp(req->body->data, "{\"key\": \"value\"}") == 0);

    cwist_http_request_destroy(req);
    printf("Passed Request Lifecycle.\n");
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

int main() {
    test_methods();
    test_request_lifecycle();
    test_response_lifecycle();
    test_parse_request();
    test_send_response();
    printf("All HTTP tests passed!\n");
    return 0;
}
