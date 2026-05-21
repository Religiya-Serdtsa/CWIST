/**
 * @file main.c
 * @brief 05-json — parse JSON body and return JSON.
 */

#include <cwist/app.h>
#include <cwist/core/utils/json_builder.h>
#include <cjson/cJSON.h>

static void echo(cwist_http_request *req, cwist_http_response *res) {
    cwist_json_builder *jb = cwist_json_builder_create();
    cwist_json_begin_object(jb);

    cJSON *in = cJSON_Parse(req->body->data);
    if (in) {
        cJSON *name = cJSON_GetObjectItem(in, "name");
        cwist_json_add_string(jb, "received",
            (name && cJSON_IsString(name)) ? name->valuestring : "?");
        cJSON_Delete(in);
    } else {
        cwist_json_add_string(jb, "error", "bad json");
    }

    cwist_json_end_object(jb);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, (char *)cwist_json_get_raw(jb));
    cwist_json_builder_destroy(jb);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_post(app, "/echo", echo);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
