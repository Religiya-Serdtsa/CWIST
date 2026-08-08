#include <cwist/app.h>
#include <cwist/sys/app/compress.h>

static void handle_data(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    /* Create a large payload that qualifies for automatic transparent compression */
    char buffer[4096];
    memset(buffer, 'A', sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    cwist_sstring_assign(res->body, buffer);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use(app, cwist_mw_compress(128));
    cwist_app_get(app, "/data", handle_data);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
