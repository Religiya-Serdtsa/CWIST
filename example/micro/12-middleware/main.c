/**
 * @file main.c
 * @brief 12-middleware — chain a logging middleware.
 */

#include <cwist/app.h>
#include <stdio.h>

static void logger(cwist_http_request *req, cwist_http_response *res,
                   cwist_handler_func next) {
    printf("[LOG] %s %s\n", cwist_http_method_to_string(req->method),
           req->path->data);
    next(req, res);
}

static void hello(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "Hello!");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use(app, logger);
    cwist_app_get(app, "/", hello);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
