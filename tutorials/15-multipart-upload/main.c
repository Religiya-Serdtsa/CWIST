#include <cwist/app.h>
#include <cwist/net/http/multipart.h>

static void handle_upload(cwist_http_request *req, cwist_http_response *res) {
    if (req->body && req->body->data) {
        /* Verify or parse multipart payload */
        cwist_sstring_assign(res->body, "{\"status\":\"success\",\"message\":\"Multipart body received\"}");
    } else {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "{\"status\":\"error\",\"message\":\"Missing request body\"}");
    }
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_post(app, "/upload", handle_upload);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
