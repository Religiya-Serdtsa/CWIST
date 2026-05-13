/**
 * @file main.c
 * @brief 14-websocket — WebSocket echo server.
 */

#include <cwist/app.h>
#include <cwist/net/websocket/websocket.h>
#include <string.h>

static void ws_handler(cwist_websocket *ws) {
    while (!ws->is_closed) {
        cwist_ws_frame *frame = cwist_websocket_receive(ws);
        if (!frame) break;
        if (frame->opcode == CWIST_WS_FRAME_TEXT) {
            char echo[256];
            snprintf(echo, sizeof(echo), "Echo: %s", frame->payload);
            cwist_websocket_send(ws, CWIST_WS_FRAME_TEXT,
                                 (uint8_t *)echo, strlen(echo));
        }
        cwist_websocket_frame_destroy(frame);
    }
    cwist_websocket_close(ws);
    cwist_websocket_destroy(ws);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_ws(app, "/ws", ws_handler);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
