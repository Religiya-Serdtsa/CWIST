#include <cwist/app.h>
#include <cwist/sys/app/waf.h>

static void handle_search(cwist_http_request *req, cwist_http_response *res) {
    const char *q = (req && req->query_params) ? cwist_query_map_get(req->query_params, "q") : NULL;
    if (q && !cwist_waf_is_safe(q, strlen(q))) {
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "{\"status\":\"error\",\"message\":\"WAF security violation detected\"}");
        return;
    }
    cwist_sstring_assign(res->body, "{\"status\":\"ok\",\"message\":\"Input clean\"}");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use(app, cwist_mw_waf_lite());
    cwist_app_get(app, "/search", handle_search);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
