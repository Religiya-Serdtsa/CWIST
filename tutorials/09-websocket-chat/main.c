#include <cwist/app.h>
#include <cwist/net/websocket/websocket.h>

static void ws_on_message(cwist_websocket *ws, const char *msg, size_t len) {
    (void)len;
    cwist_websocket_send_text(ws, "Echo: ");
    cwist_websocket_send_text(ws, msg);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_ws(app, "/ws", ws_on_message);
    cwist_app_listen(app, 8088);
    cwist_app_destroy(app);
    return 0;
}
