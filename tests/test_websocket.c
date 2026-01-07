#include <cwist/net/websocket/websocket.h>
#include <cwist/net/http/http.h>
#include <cwist/core/sstring/sstring.h>
#include "../src/net/websocket/ws_utils.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

void print_hex(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02x ", data[i]);
    }
    printf("\n");
}

void test_sha1() {
    printf("Testing SHA1...\n");
    const char *input = "abc";
    uint8_t hash[20];
    sha1((const uint8_t *)input, strlen(input), hash);
    
    printf("Computed Hash: ");
    print_hex(hash, 20);
    
    // Expected for "abc": a9993e36 4706816a ba3e2571 7850c26c 9cd0d89d
    uint8_t expected[] = {
        0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e, 
        0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d
    };
    
    assert(memcmp(hash, expected, 20) == 0);
    printf("SHA1 Test Passed.\n");
}

void test_handshake_key_generation() {
    test_sha1();
    printf("Testing Handshake Key Generation...\n");
    
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return;
    }
    
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_header_add(&req->headers, "Connection", "Upgrade");
    cwist_http_header_add(&req->headers, "Upgrade", "websocket");
    cwist_http_header_add(&req->headers, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
    
    cwist_websocket *ws = cwist_websocket_upgrade(req, sv[0]);
    assert(ws != NULL);
    assert(ws->fd == sv[0]);
    assert(req->upgraded == true);
    
    // Read response from sv[1]
    char buffer[1024];
    int len = read(sv[1], buffer, sizeof(buffer)-1);
    buffer[len] = '\0';
    
    printf("Response:\n%s\n", buffer);
    
    assert(strstr(buffer, "101 Switching Protocols") != NULL);
    assert(strstr(buffer, "Upgrade: websocket") != NULL);
    assert(strstr(buffer, "Connection: Upgrade") != NULL);
    assert(strstr(buffer, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);
    
    cwist_websocket_destroy(ws);
    cwist_http_request_destroy(req);
    close(sv[0]);
    close(sv[1]);
    
    printf("Handshake Test Passed.\n");
}

int main() {
    test_handshake_key_generation();
    return 0;
}
