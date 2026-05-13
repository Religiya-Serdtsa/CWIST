/**
 * @file main.c
 * @brief JSON REST API using the app-level routing API.
 *
 * No manual request parsing or response serialization — just handlers
 * attached to paths via cwist_app_get / cwist_app_post.
 */

#include <cwist/app.h>
#include <cwist/core/utils/json_builder.h>
#include <cjson/cJSON.h>

/* GET /users — return a JSON array of mock users */
static void get_users(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_json_builder *jb = cwist_json_builder_create();
    cwist_json_begin_array(jb, NULL);

    cwist_json_begin_object(jb);
    cwist_json_add_int(jb, "id", 1);
    cwist_json_add_string(jb, "name", "Alice");
    cwist_json_add_bool(jb, "active", true);
    cwist_json_end_object(jb);

    cwist_json_begin_object(jb);
    cwist_json_add_int(jb, "id", 2);
    cwist_json_add_string(jb, "name", "Bob");
    cwist_json_add_bool(jb, "active", false);
    cwist_json_end_object(jb);

    cwist_json_end_array(jb);

    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, (char *)cwist_json_get_raw(jb));
    cwist_json_builder_destroy(jb);
}

/* POST /users — parse JSON body and echo it back */
static void create_user(cwist_http_request *req, cwist_http_response *res) {
    cwist_json_builder *jb = cwist_json_builder_create();
    cwist_json_begin_object(jb);

    if (req->body && req->body->data[0] != '\0') {
        cJSON *parsed = cJSON_Parse(req->body->data);
        if (parsed) {
            cJSON *name = cJSON_GetObjectItem(parsed, "name");
            cwist_json_add_string(jb, "status", "created");
            cwist_json_add_string(jb, "name",
                (name && cJSON_IsString(name)) ? name->valuestring : "unknown");
            cJSON_Delete(parsed);
        } else {
            cwist_json_add_string(jb, "status", "error");
            cwist_json_add_string(jb, "message", "invalid JSON body");
        }
    } else {
        cwist_json_add_string(jb, "status", "error");
        cwist_json_add_string(jb, "message", "empty body");
    }

    cwist_json_end_object(jb);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, (char *)cwist_json_get_raw(jb));
    cwist_json_builder_destroy(jb);
    res->status_code = CWIST_HTTP_CREATED;
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/users", get_users);
    cwist_app_post(app, "/users", create_user);
    cwist_app_listen(app, 8083);
    cwist_app_destroy(app);
    return 0;
}
