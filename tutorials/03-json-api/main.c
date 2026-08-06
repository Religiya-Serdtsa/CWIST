#include <cwist/app.h>
#include <cwist/core/utils/json_builder.h>

static void handle_json(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", "success");
    cJSON_AddNumberToObject(json, "code", 200);

    char *json_str = cJSON_PrintUnformatted(json);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, json_str);

    free(json_str);
    cJSON_Delete(json);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/api/data", handle_json);
    cwist_app_listen(app, 8082);
    cwist_app_destroy(app);
    return 0;
}
