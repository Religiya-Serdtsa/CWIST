#include <cwist/app.h>
#include <cwist/net/http/sse.h>

static void handle_events(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sse_init(res);
    cwist_sse_send(res, "message", "Welcome to Live SSE Stream");
    cwist_sse_send(res, "update", "Tick 1");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/events", handle_events);
    cwist_app_listen(app, 8087);
    cwist_app_destroy(app);
    return 0;
}
