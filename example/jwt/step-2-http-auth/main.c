/**
 * @file main.c
 * @brief JWT authentication using the app-level API.
 *
 * No manual socket or HTTP framing code — just route handlers.
 */

#include <cwist/app.h>
#include <cwist/security/jwt/jwt.h>
#include <cwist/core/utils/json_builder.h>
#include <cwist/core/mem/alloc.h>

#define JWT_SECRET "change-me-in-production"

/* POST /login — issue a token */
static void login(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_json_builder *jb = cwist_json_builder_create();
    cwist_json_begin_object(jb);

    const char *user = "demo";
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"sub\":\"1\",\"user\":\"%s\"}", user);

    char *token = cwist_jwt_sign(payload, JWT_SECRET, 3600);
    if (token) {
        cwist_json_add_string(jb, "token", token);
        cwist_free(token);
        res->status_code = CWIST_HTTP_OK;
    } else {
        cwist_json_add_string(jb, "error", "token generation failed");
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
    }

    cwist_json_end_object(jb);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, (char *)cwist_json_get_raw(jb));
    cwist_json_builder_destroy(jb);
}

/* GET /profile — protected endpoint, requires Bearer token */
static void profile(cwist_http_request *req, cwist_http_response *res) {
    cwist_json_builder *jb = cwist_json_builder_create();
    cwist_json_begin_object(jb);

    char *auth = cwist_http_header_get(req->headers, "Authorization");
    if (!auth || strncmp(auth, "Bearer ", 7) != 0) {
        cwist_json_add_string(jb, "error", "missing or malformed Authorization header");
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        goto done;
    }

    cwist_jwt_claims *claims = cwist_jwt_verify(auth + 7, JWT_SECRET);
    if (!claims) {
        cwist_json_add_string(jb, "error", "invalid or expired token");
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        goto done;
    }

    const char *user = cwist_jwt_claims_get(claims, "user");
    cwist_json_add_string(jb, "user", user ? user : "unknown");
    cwist_json_add_string(jb, "message", "Welcome to your profile!");
    cwist_jwt_claims_destroy(claims);
    res->status_code = CWIST_HTTP_OK;

done:
    cwist_json_end_object(jb);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, (char *)cwist_json_get_raw(jb));
    cwist_json_builder_destroy(jb);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_post(app, "/login", login);
    cwist_app_get(app, "/profile", profile);
    cwist_app_listen(app, 8084);
    cwist_app_destroy(app);
    return 0;
}
