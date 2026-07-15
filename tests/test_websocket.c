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

static void send_masked_binary_frame(int fd, const uint8_t *payload, size_t len) {
    assert(len < 126);
    uint8_t frame[140];
    size_t pos = 0;
    frame[pos++] = 0x82; /* FIN=1, BINARY */
    frame[pos++] = (uint8_t)(0x80 | len);
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(frame + pos, mask, 4);
    pos += 4;
    for (size_t i = 0; i < len; i++) {
        frame[pos++] = payload[i] ^ mask[i % 4];
    }
    assert(write(fd, frame, pos) == (ssize_t)pos);
}

static void test_websocket_sequenced(void) {
    printf("Testing WebSocket sequenced send/receive...\n");

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_header_add(&req->headers, "Connection", "Upgrade");
    cwist_http_header_add(&req->headers, "Upgrade", "websocket");
    cwist_http_header_add(&req->headers, "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");

    cwist_websocket *ws = cwist_websocket_upgrade(req, sv[0]);
    assert(ws != NULL);

    const char *message = "CWIST-sequenced-websocket-message";
    size_t msg_len = strlen(message);
    assert(cwist_websocket_send_sequenced(ws, (const uint8_t *)message, msg_len, 8) == 0);

    /* Read frames from sv[1] and verify they carry sequence chunks. */
    size_t total_payload = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t head[2];
        assert(read(sv[1], head, 2) == 2);
        assert(head[0] == 0x82); /* FIN binary, server does not mask */
        assert((head[1] & 0x80) == 0);
        uint8_t plen = head[1] & 0x7f;
        uint8_t payload[64];
        assert(read(sv[1], payload, plen) == plen);
        cwist_seq_chunk_t chunk;
        assert(cwist_seq_chunk_parse(payload, plen, &chunk));
        assert(chunk.total > 1 || chunk.seq == 1);
        total_payload += chunk.payload_len;
        if (chunk.seq == chunk.total) break;
    }
    assert(total_payload == msg_len);

    /* Now test receive_sequenced: send masked binary frames out of order. */
    cwist_seq_message_t msg;
    assert(cwist_seq_split((const uint8_t *)"hello cwist", 11, 4, &msg));
    assert(msg.count == 3);
    /* Send chunks 2, 0, 1 (out of order). */
    send_masked_binary_frame(sv[1], msg.chunks[1], msg.chunk_lens[1]);
    send_masked_binary_frame(sv[1], msg.chunks[0], msg.chunk_lens[0]);
    send_masked_binary_frame(sv[1], msg.chunks[2], msg.chunk_lens[2]);

    size_t received_len = 0;
    uint8_t *received = cwist_websocket_receive_sequenced(ws, &received_len);
    assert(received != NULL);
    assert(received_len == 11);
    assert(memcmp(received, "hello cwist", 11) == 0);

    cwist_free(received);
    cwist_seq_message_free(&msg);
    cwist_websocket_destroy(ws);
    cwist_http_request_destroy(req);
    close(sv[0]);
    close(sv[1]);
    printf("WebSocket sequenced test passed.\n");
}

int main() {
    test_handshake_key_generation();
    test_websocket_sequenced();
    return 0;
}
