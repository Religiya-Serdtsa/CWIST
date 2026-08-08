#include <cwist/app.h>
#include <cwist/sys/app/csrf.h>

static void handle_form(cwist_http_request *req, cwist_http_response *res) {
    const char *token = cwist_csrf_token(req);
    char body[512];
    snprintf(body, sizeof(body),
             "<html><body>"
             "<h1>CSRF Protection Demo</h1>"
             "<form method=\"POST\" action=\"/submit\">"
             "<input type=\"hidden\" name=\"_csrf\" value=\"%s\"/>"
             "<input type=\"text\" name=\"data\" value=\"test\"/>"
             "<button type=\"submit\">Submit</button>"
             "</form>"
             "</body></html>",
             token ? token : "");
    cwist_sstring_assign(res->body, body);
}

static void handle_submit(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "{\"status\":\"success\",\"message\":\"CSRF token validated successfully\"}");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use(app, cwist_mw_csrf(app));
    cwist_app_get(app, "/", handle_form);
    cwist_app_post(app, "/submit", handle_submit);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
