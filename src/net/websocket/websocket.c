#define _GNU_SOURCE
#include <cwist/net/websocket/websocket.h>
#include <cwist/net/http/http.h>
#include <cwist/core/mem/alloc.h>
#include "ws_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

cwist_websocket *cwist_websocket_upgrade(cwist_http_request *req, int client_fd) {
    if (!req || client_fd < 0) return NULL;

    // Validate Headers
    char *connection = cwist_http_header_get(req->headers, "Connection");
    char *upgrade = cwist_http_header_get(req->headers, "Upgrade");
    char *key = cwist_http_header_get(req->headers, "Sec-WebSocket-Key");

    if (!connection || !upgrade || !key) return NULL;
    if (strcasestr(connection, "Upgrade") == NULL) return NULL;
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

static ssize_t read_exact(int fd, void *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(fd, (uint8_t *)buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return total;
}

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

void cwist_websocket_frame_destroy(cwist_ws_frame *frame) {
    if (frame) {
        if (frame->payload) cwist_free(frame->payload);
        cwist_free(frame);
    }
}

void cwist_websocket_close(cwist_websocket *ws) {
    if (ws && !ws->is_closed) {
        cwist_websocket_send(ws, CWIST_WS_FRAME_CLOSE, NULL, 0);
        ws->is_closed = true;
    }
}

void cwist_websocket_destroy(cwist_websocket *ws) {
    if (ws) {
        // We don't own fd in terms of closing it immediately if the app wants to, 
        // but typically destroying WS wrapper implies we are done.
        // The App handler owns the FD usually.
        cwist_free(ws);
    }
}
