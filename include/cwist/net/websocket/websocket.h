/**
 * @file websocket.h
 * @brief WebSocket upgrade and frame handling.
 */

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

/**
 * @brief Send a frame with the specified opcode and payload.
 */
int cwist_websocket_send(cwist_websocket *ws, cwist_ws_opcode_t opcode, const uint8_t *data, size_t len);

/**
 * @brief Send a large payload as sequenced binary frames.
 *
 * Splits @p data into chunks of @p chunk_payload_size (or smaller for the
 * final chunk), prefixes each chunk with the CWIST sequence header, and sends
 * every chunk as an independent binary frame.  The peer can reassemble the
 * original payload even when frames arrive out of order.
 *
 * @param ws WebSocket connection.
 * @param data Payload bytes.
 * @param len Payload length.
 * @param chunk_payload_size Maximum payload bytes per chunk (must be > 0).
 * @return 0 on success, -1 on failure.
 */
int cwist_websocket_send_sequenced(cwist_websocket *ws,
                                   const uint8_t *data,
                                   size_t len,
                                   uint16_t chunk_payload_size);

/**
 * @brief Receive and reassemble a sequenced binary message.
 *
 * Reads binary frames until every chunk of one sequenced message has arrived,
 * discarding duplicates and reordering out-of-order chunks.  The returned
 * buffer is heap-allocated and must be freed with cwist_free().
 *
 * @param ws WebSocket connection.
 * @param out_len Receives the length of the reassembled payload.
 * @return Newly allocated payload buffer, or NULL on error/incomplete message.
 */
uint8_t *cwist_websocket_receive_sequenced(cwist_websocket *ws, size_t *out_len);

/**
 * @brief Destroy a frame.
 */
void cwist_websocket_frame_destroy(cwist_ws_frame *frame);

/**
 * @brief Close the WebSocket connection.
 */
void cwist_websocket_close(cwist_websocket *ws);

/**
 * @brief Destroy the WebSocket context.
 */
void cwist_websocket_destroy(cwist_websocket *ws);

#endif
