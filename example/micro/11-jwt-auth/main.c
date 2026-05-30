/**
 * @file main.c
 * @brief 11-jwt-auth — issue and verify Bearer tokens.
 */

#include <cwist/app.h>
#include <cwist/security/jwt/jwt.h>
#include <cwist/core/utils/json_builder.h>
#include <cwist/core/mem/alloc.h>
#include <string.h>

#define SECRET "change-me"

static void login(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    char *token = cwist_jwt_sign("{\"sub\":\"1\",\"user\":\"alice\"}", SECRET, 3600);
    cwist_json_builder *jb = cwist_json_builder_create();
    cwist_json_begin_object(jb);
    cwist_json_add_string(jb, "token", token ? token : "");
    cwist_json_end_object(jb);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, (char *)cwist_json_get_raw(jb));
    cwist_json_builder_destroy(jb);
    cwist_free(token);
}

static void dashboard(cwist_http_request *req, cwist_http_response *res) {
    char *auth = cwist_http_header_get(req->headers, "Authorization");
    if (!auth || strncmp(auth, "Bearer ", 7) != 0) {
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->body, "Unauthorized");
        return;
    }
    cwist_jwt_claims *claims = cwist_jwt_verify(auth + 7, SECRET);
    if (!claims) {
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->body, "Invalid token");
        return;
    }
    cwist_sstring_assign(res->body, "Welcome, ");
    cwist_sstring_append(res->body, cwist_jwt_claims_get(claims, "user"));
    cwist_jwt_claims_destroy(claims);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_post(app, "/login", login);
    cwist_app_get(app, "/dashboard", dashboard);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
