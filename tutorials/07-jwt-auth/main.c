#include <cwist/app.h>
#include <cwist/security/jwt/jwt.h>

static void handle_token(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    const char *secret = "super-secret-key-1234";

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "user", "admin");
    cJSON_AddNumberToObject(payload, "exp", 1999999999);

    char *token = cwist_jwt_sign(payload, secret);
    cJSON_Delete(payload);

    if (token) {
        cwist_sstring_assign(res->body, token);
        free(token);
    } else {
        cwist_sstring_assign(res->body, "JWT sign failed");
    }
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/token", handle_token);
    cwist_app_listen(app, 8086);
    cwist_app_destroy(app);
    return 0;
}
