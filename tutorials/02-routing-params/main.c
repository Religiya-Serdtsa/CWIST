#include <cwist/app.h>

static void handle_user(cwist_http_request *req, cwist_http_response *res) {
    const char *id = (req && req->path_params) ? cwist_query_map_get(req->path_params, "id") : NULL;
    if (id) {
        cwist_sstring_assign(res->body, "User Profile ID: ");
        cwist_sstring_append(res->body, id);
    } else {
        cwist_sstring_assign(res->body, "User Directory");
    }
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/users/:id", handle_user);
    cwist_app_listen(app, 8081);
    cwist_app_destroy(app);
    return 0;
}
