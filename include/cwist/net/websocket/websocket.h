#ifndef __CWIST_WEBSOCKET_H__
#define __CWIST_WEBSOCKET_H__

#include <cwist/net/http/http.h>
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

/**
 * @brief Upgrade a standard HTTP request to a WebSocket connection.
 * @return `NULL` if the handshake fails or the request is invalid, otherwise the upgraded context.
 */
cwist_websocket *cwist_websocket_upgrade(cwist_http_request *req, int client_fd);

/**
 * @brief Receive a frame, blocking until data arrives or the socket closes.
 * @return `NULL` on error or if the connection was closed.
 */
cwist_ws_frame *cwist_websocket_receive(cwist_websocket *ws);

/** @brief Send a frame with the specified opcode and payload. */
int cwist_websocket_send(cwist_websocket *ws, cwist_ws_opcode_t opcode, const uint8_t *data, size_t len);

void cwist_websocket_frame_destroy(cwist_ws_frame *frame);
void cwist_websocket_close(cwist_websocket *ws);
void cwist_websocket_destroy(cwist_websocket *ws);

#endif
