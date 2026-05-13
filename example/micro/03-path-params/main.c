/**
 * @file main.c
 * @brief 03-path-params — read :id from the URL.
 */

#include <cwist/app.h>
#include <cwist/net/http/query.h>
#include <stdio.h>

static void show(cwist_http_request *req, cwist_http_response *res) {
    const char *id = cwist_query_map_get(req->path_params, "id");
    char buf[64];
    snprintf(buf, sizeof(buf), "Post ID: %s", id ? id : "unknown");
    cwist_sstring_assign(res->body, buf);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/posts/:id", show);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
