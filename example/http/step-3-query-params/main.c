/**
 * @file main.c
 * @brief Query parameter access through the app API.
 *
 * Query params are parsed automatically; handlers just read them.
 */

#include <cwist/app.h>
#include <cwist/net/http/query.h>
#include <cwist/core/sstring/sstring.h>

static void search(cwist_http_request *req, cwist_http_response *res) {
    cwist_sstring *reply = cwist_sstring_create();
    cwist_sstring_assign(reply, "Query parameters received:\n");

    if (req->query_params) {
        const char *q    = cwist_query_map_get(req->query_params, "q");
        const char *page = cwist_query_map_get(req->query_params, "page");
        const char *sort = cwist_query_map_get(req->query_params, "sort");

        if (q)    { cwist_sstring_append(reply, "  q    = "); cwist_sstring_append(reply, q);    cwist_sstring_append(reply, "\n"); }
        if (page) { cwist_sstring_append(reply, "  page = "); cwist_sstring_append(reply, page); cwist_sstring_append(reply, "\n"); }
        if (sort) { cwist_sstring_append(reply, "  sort = "); cwist_sstring_append(reply, sort); cwist_sstring_append(reply, "\n"); }

        if (!q && !page && !sort)
            cwist_sstring_append(reply, "  (none)\n");
    } else {
        cwist_sstring_append(reply, "  (no query string)\n");
    }

    cwist_sstring_append(reply, "\nTry: curl 'http://localhost:8082/search?q=hello&page=2&sort=asc'\n");
    cwist_sstring_assign(res->body, reply->data);
    cwist_sstring_destroy(reply);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/search", search);
    cwist_app_listen(app, 8082);
    cwist_app_destroy(app);
    return 0;
}
