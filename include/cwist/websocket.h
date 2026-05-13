#ifndef __CWIST_WEBSOCKET_H__
#define __CWIST_WEBSOCKET_H__

#include <cwist/http.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct cwist_websocket {
    int fd;
    bool is_closed;
} cwist_websocket;

typedef enum {
    CWIST_WS_FRAME_CONTINUATION = 0x0,
    CWIST_WS_FRAME_TEXT = 0x1,
    CWIST_WS_FRAME_BINARY = 0x2,
    CWIST_WS_FRAME_CLOSE = 0x8,
    CWIST_WS_FRAME_PING = 0x9,
    CWIST_WS_FRAME_PONG = 0xA
} cwist_ws_opcode_t;

typedef struct cwist_ws_frame {
    bool fin;
    cwist_ws_opcode_t opcode;
    uint8_t *payload;
    size_t payload_len;
} cwist_ws_frame;

// Upgrade a standard HTTP request to a WebSocket connection.
// Returns NULL if the handshake fails or the request is not a valid WebSocket upgrade.
// If successful, sends the 101 Switching Protocols response and returns the WS context.
cwist_websocket *cwist_websocket_upgrade(cwist_http_request *req, int client_fd);

// Receive a frame. Blocks until a frame is received or connection closed.
// Returns NULL on error or connection close.
cwist_ws_frame *cwist_websocket_receive(cwist_websocket *ws);

// Send a frame.
int cwist_websocket_send(cwist_websocket *ws, cwist_ws_opcode_t opcode, const uint8_t *data, size_t len);

void cwist_websocket_frame_destroy(cwist_ws_frame *frame);
void cwist_websocket_close(cwist_websocket *ws);
void cwist_websocket_destroy(cwist_websocket *ws);

#endif
