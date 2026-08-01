#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cwist/net/websocket/websocket.h>
#include <cwist/net/http/http.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/seq/seq.h>
#include "ws_utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>

/**
 * @file websocket.c
 * @brief WebSocket handshake and frame transport helpers for CWIST handlers.
 */

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/**
 * @brief Portable case-insensitive substring search.
 *
 * strcasestr() is a GNU extension that does not exist on macOS, so keep a
 * small ASCII-only variant for header validation.
 */
static char *ws_strcasestr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n &&
               tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++;
            n++;
        }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

/**
 * @brief Upgrade an HTTP request to a WebSocket connection.
 *
 * The function validates the required upgrade headers, computes the
 * Sec-WebSocket-Accept response, emits the 101 Switching Protocols reply, and
 * returns a lightweight wrapper around the already-open client socket.
 *
 * @param req Parsed HTTP request that requested the upgrade.
 * @param client_fd Accepted client socket descriptor.
 * @return WebSocket wrapper on success, or NULL when the handshake fails.
 */
cwist_websocket *cwist_websocket_upgrade(cwist_http_request *req, int client_fd) {
    if (!req || client_fd < 0) return NULL;

    // Validate Headers
    char *connection = cwist_http_header_get(req->headers, "Connection");
    char *upgrade = cwist_http_header_get(req->headers, "Upgrade");
    char *key = cwist_http_header_get(req->headers, "Sec-WebSocket-Key");

    if (!connection || !upgrade || !key) return NULL;
    if (ws_strcasestr(connection, "Upgrade") == NULL) return NULL;
    if (strcasecmp(upgrade, "websocket") != 0) return NULL;

    // Handshake Key Generation
    char combined_key[512];
    snprintf(combined_key, sizeof(combined_key), "%s%s", key, WS_GUID);

    uint8_t hash[20];
    sha1((uint8_t *)combined_key, strlen(combined_key), hash);

    size_t accept_len;
    char *accept_key = base64_encode(hash, 20, &accept_len);
    if (!accept_key) return NULL;

    // Send Response
    cwist_http_response *res = cwist_http_response_create();
    res->status_code = 101;
    cwist_sstring_assign(res->status_text, "Switching Protocols");
    cwist_http_header_add(&res->headers, "Upgrade", "websocket");
    cwist_http_header_add(&res->headers, "Connection", "Upgrade");
    cwist_http_header_add(&res->headers, "Sec-WebSocket-Accept", accept_key);

    cwist_error_t err = cwist_http_send_response(client_fd, res);
    
    cwist_http_response_destroy(res);
    cwist_free(accept_key);

    if (err.error.err_i16 != 0) return NULL;

    req->upgraded = true;

    cwist_websocket *ws = (cwist_websocket *)cwist_alloc(sizeof(cwist_websocket));
    ws->fd = client_fd;
    ws->is_closed = false;
    return ws;
}

/**
 * @brief Read exactly @p len bytes from a socket unless the peer closes first.
 * @param fd Socket descriptor to read from.
 * @param buf Destination buffer.
 * @param len Number of bytes required to complete the operation.
 * @return Number of bytes read, or -1 when the stream cannot satisfy the request.
 */
static ssize_t read_exact(int fd, void *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(fd, (uint8_t *)buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return total;
}

/**
 * @brief Receive the next WebSocket frame from a connected client.
 *
 * Client frames are required to be masked by RFC 6455, so unmasked payloads are
 * rejected. Payload data is copied into a heap buffer and null-terminated for
 * convenience when the caller interprets a text frame as a C string.
 *
 * @param ws WebSocket connection wrapper returned by cwist_websocket_upgrade().
 * @return Newly allocated frame, or NULL when the connection is closed or invalid.
 */
cwist_ws_frame *cwist_websocket_receive(cwist_websocket *ws) {
    if (!ws || ws->is_closed) return NULL;

    uint8_t head[2];
    if (read_exact(ws->fd, head, 2) < 0) return NULL;

    bool fin = (head[0] & 0x80) != 0;
    cwist_ws_opcode_t opcode = head[0] & 0x0F;
    bool masked = (head[1] & 0x80) != 0;
    uint64_t payload_len = head[1] & 0x7F;

    if (!masked) {
        // Client-to-server frames must be masked per spec
        // We can choose to strict close or allow. Strict is better.
        // For now, let's just return error.
        return NULL;
    }

    if (payload_len == 126) {
        uint16_t len16;
        if (read_exact(ws->fd, &len16, 2) < 0) return NULL;
        payload_len = ntohs(len16);
    } else if (payload_len == 127) {
        uint64_t len64;
        if (read_exact(ws->fd, &len64, 8) < 0) return NULL;
        // manually swap if no be64toh
        // assuming be64toh or similar exists, or manual
        // Linux usually has be64toh in <endian.h>
        // Let's implement manual swap to be portable
        uint8_t *p = (uint8_t *)&len64;
        payload_len = ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
                      ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
                      ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
                      ((uint64_t)p[6] << 8)  | ((uint64_t)p[7]);
    }

    uint8_t masking_key[4];
    if (read_exact(ws->fd, masking_key, 4) < 0) return NULL;

    uint8_t *payload = NULL;
    if (payload_len > 0) {
        payload = (uint8_t *)cwist_alloc(payload_len + 1); // +1 for safety null term if text
        if (!payload) return NULL;
        if (read_exact(ws->fd, payload, payload_len) < 0) {
            cwist_free(payload);
            return NULL;
        }

        // Unmask
        for (uint64_t i = 0; i < payload_len; i++) {
            payload[i] ^= masking_key[i % 4];
        }
        payload[payload_len] = '\0'; // Null terminate for convenience if text
    }

    if (opcode == CWIST_WS_FRAME_CLOSE) {
        ws->is_closed = true;
    }

    cwist_ws_frame *frame = (cwist_ws_frame *)cwist_alloc(sizeof(cwist_ws_frame));
    frame->fin = fin;
    frame->opcode = opcode;
    frame->payload = payload;
    frame->payload_len = payload_len;

    return frame;
}

/**
 * @brief Send a single FIN-terminated WebSocket frame to the peer.
 * @param ws Active WebSocket connection wrapper.
 * @param opcode WebSocket opcode describing the payload semantics.
 * @param data Optional payload buffer. May be NULL when @p len is zero.
 * @param len Number of payload bytes to transmit.
 * @return 0 on success, or -1 when the socket write fails.
 */
int cwist_websocket_send(cwist_websocket *ws, cwist_ws_opcode_t opcode, const uint8_t *data, size_t len) {
    if (!ws || ws->is_closed) return -1;

    uint8_t head[10]; // Max header size (2 + 8)
    size_t head_len = 2;

    head[0] = 0x80 | (opcode & 0x0F); // FIN=1

    if (len < 126) {
        head[1] = len;
    } else if (len < 65536) {
        head[1] = 126;
        head[2] = (len >> 8) & 0xFF;
        head[3] = len & 0xFF;
        head_len += 2;
    } else {
        head[1] = 127;
        for (int i = 0; i < 8; i++) {
            head[2 + i] = (len >> ((7 - i) * 8)) & 0xFF;
        }
        head_len += 8;
    }

    // Server does not mask frames

    if (send(ws->fd, head, head_len, 0) < 0) return -1;
    if (len > 0) {
        if (send(ws->fd, data, len, 0) < 0) return -1;
    }

    return 0;
}

/**
 * @brief Release a frame and its owned payload buffer.
 * @param frame Frame object to destroy. NULL is ignored.
 */
void cwist_websocket_frame_destroy(cwist_ws_frame *frame) {
    if (frame) {
        if (frame->payload) cwist_free(frame->payload);
        cwist_free(frame);
    }
}

/**
 * @brief Send a large payload as sequenced binary frames.
 */
int cwist_websocket_send_sequenced(cwist_websocket *ws,
                                   const uint8_t *data,
                                   size_t len,
                                   uint16_t chunk_payload_size) {
    if (!ws || ws->is_closed || !data || len == 0 || chunk_payload_size == 0) return -1;

    cwist_seq_message_t msg;
    if (!cwist_seq_split(data, len, chunk_payload_size, &msg)) return -1;

    int rc = 0;
    for (size_t i = 0; i < msg.count && rc == 0; i++) {
        if (cwist_websocket_send(ws, CWIST_WS_FRAME_BINARY, msg.chunks[i], msg.chunk_lens[i]) != 0) {
            rc = -1;
        }
    }

    cwist_seq_message_free(&msg);
    return rc;
}

/**
 * @brief Receive and reassemble a sequenced binary message.
 */
uint8_t *cwist_websocket_receive_sequenced(cwist_websocket *ws, size_t *out_len) {
    if (!out_len) return NULL;
    *out_len = 0;
    if (!ws || ws->is_closed) return NULL;

    cwist_seq_assembler_t *a = cwist_seq_assembler_create();
    if (!a) return NULL;

    uint8_t *result = NULL;
    while (!cwist_seq_assembler_is_complete(a)) {
        cwist_ws_frame *frame = cwist_websocket_receive(ws);
        if (!frame) break;

        if (frame->opcode == CWIST_WS_FRAME_BINARY && frame->payload && frame->payload_len > 0) {
            cwist_seq_chunk_t chunk;
            if (cwist_seq_chunk_parse(frame->payload, frame->payload_len, &chunk)) {
                cwist_seq_assembler_feed(a, &chunk);
            }
        }

        bool is_close = (frame->opcode == CWIST_WS_FRAME_CLOSE);
        cwist_websocket_frame_destroy(frame);
        if (is_close) break;
    }

    const uint8_t *assembled = NULL;
    size_t assembled_len = 0;
    if (cwist_seq_assembler_get_data(a, &assembled, &assembled_len) && assembled_len > 0) {
        result = (uint8_t *)cwist_alloc(assembled_len + 1);
        if (result) {
            memcpy(result, assembled, assembled_len);
            result[assembled_len] = '\0';
            *out_len = assembled_len;
        }
    }

    cwist_seq_assembler_destroy(a);
    return result;
}

/**
 * @brief Initiate a close handshake for the WebSocket wrapper.
 * @param ws Connection to close. NULL is ignored.
 */
void cwist_websocket_close(cwist_websocket *ws) {
    if (ws && !ws->is_closed) {
        cwist_websocket_send(ws, CWIST_WS_FRAME_CLOSE, NULL, 0);
        ws->is_closed = true;
    }
}

/**
 * @brief Destroy the WebSocket wrapper without closing the underlying socket.
 * @param ws Wrapper to release. NULL is ignored.
 */
void cwist_websocket_destroy(cwist_websocket *ws) {
    if (ws) {
        // We don't own fd in terms of closing it immediately if the app wants to, 
        // but typically destroying WS wrapper implies we are done.
        // The App handler owns the FD usually.
        cwist_free(ws);
    }
}
