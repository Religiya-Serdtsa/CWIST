#include <cwist/app.h>
#include <stdio.h>

static void logging_middleware(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    printf("[LOG] Processing request path: %s\n", req->path ? req->path->data : "/");
    if (next) next(req, res);
}

static void handle_dashboard(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "Protected Dashboard Area");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use(app, logging_middleware);
    cwist_app_get(app, "/dashboard", handle_dashboard);
    cwist_app_listen(app, 8083);
    cwist_app_destroy(app);
    return 0;
}
