/**
 * @file main.c
 * @brief 02-routes — GET and POST handlers.
 */

#include <cwist/app.h>

static void list(cwist_http_request *req, cwist_http_response *res) {
    (void)req; cwist_sstring_assign(res->body, "LIST all posts");
}
static void create(cwist_http_request *req, cwist_http_response *res) {
    (void)req; cwist_sstring_assign(res->body, "CREATE a post");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app,  "/posts", list);
    cwist_app_post(app, "/posts", create);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
