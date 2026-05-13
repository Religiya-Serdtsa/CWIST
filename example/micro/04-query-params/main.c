/**
 * @file main.c
 * @brief 04-query-params — read ?page=1&limit=10.
 */

#include <cwist/app.h>
#include <cwist/net/http/query.h>
#include <stdio.h>

static void index_handler(cwist_http_request *req, cwist_http_response *res) {
    const char *page  = cwist_query_map_get(req->query_params, "page");
    const char *limit = cwist_query_map_get(req->query_params, "limit");
    char buf[128];
    snprintf(buf, sizeof(buf), "page=%s  limit=%s",
             page ? page : "1", limit ? limit : "10");
    cwist_sstring_assign(res->body, buf);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/posts", index_handler);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
