#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/mux.h>
#include <cwist/security/jwt/jwt.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/utils/json_builder.h>
#include <cwist/core/mem/alloc.h>

#define PORT       8084
#define JWT_SECRET "change-me-in-production"

/* POST /login — issue a token */
static void login(cwist_http_request *req, cwist_http_response *res) {
    cwist_json_builder *jb = cwist_json_builder_create();
    cwist_json_begin_object(jb);

    /* In a real app: validate credentials from the body/DB */
    const char *user = "demo";
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"sub\":\"1\",\"user\":\"%s\"}", user);

    char *token = cwist_jwt_sign(payload, JWT_SECRET, 3600);
    if (token) {
        cwist_json_add_string(jb, "token", token);
        cwist_free(token);
        res->status_code = CWIST_HTTP_OK;
        cwist_sstring_assign(res->status_text, "OK");
    } else {
        cwist_json_add_string(jb, "error", "token generation failed");
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->status_text, "Internal Server Error");
    }

    cwist_json_end_object(jb);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, (char *)cwist_json_get_raw(jb));
    cwist_json_builder_destroy(jb);
    (void)req;
}

/* GET /profile — protected endpoint, requires Bearer token */
static void profile(cwist_http_request *req, cwist_http_response *res) {
    cwist_json_builder *jb = cwist_json_builder_create();
    cwist_json_begin_object(jb);

    char *auth = cwist_http_header_get(req->headers, "Authorization");
    if (!auth || strncmp(auth, "Bearer ", 7) != 0) {
        cwist_json_add_string(jb, "error", "missing or malformed Authorization header");
        cwist_json_end_object(jb);
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->status_text, "Unauthorized");
        cwist_http_header_add(&res->headers, "Content-Type", "application/json");
        cwist_sstring_assign(res->body, (char *)cwist_json_get_raw(jb));
        cwist_json_builder_destroy(jb);
        return;
    }

    const char *raw_token = auth + 7;
    cwist_jwt_claims *claims = cwist_jwt_verify(raw_token, JWT_SECRET);
    if (!claims) {
        cwist_json_add_string(jb, "error", "invalid or expired token");
        cwist_json_end_object(jb);
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->status_text, "Unauthorized");
        cwist_http_header_add(&res->headers, "Content-Type", "application/json");
        cwist_sstring_assign(res->body, (char *)cwist_json_get_raw(jb));
        cwist_json_builder_destroy(jb);
        return;
    }

    const char *user = cwist_jwt_claims_get(claims, "user");
    cwist_json_add_string(jb, "user", user ? user : "unknown");
    cwist_json_add_string(jb, "message", "Welcome to your profile!");
    cwist_jwt_claims_destroy(claims);

    cwist_json_end_object(jb);
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->status_text, "OK");
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, (char *)cwist_json_get_raw(jb));
    cwist_json_builder_destroy(jb);
}

/* ---- Server glue ---- */
typedef struct { cwist_mux_router *router; } ctx_t;

static void handle_client(int client_fd, void *arg) {
    ctx_t *ctx = (ctx_t *)arg;
    char *buf  = cwist_alloc(CWIST_HTTP_READ_BUFFER_SIZE);
    if (!buf) { close(client_fd); return; }
    size_t buf_len = 0;
    cwist_http_request *req =
        cwist_http_receive_request(client_fd, buf, CWIST_HTTP_READ_BUFFER_SIZE, &buf_len);
    if (!req) { cwist_free(buf); close(client_fd); return; }
    printf("[%s] %s\n", cwist_http_method_to_string(req->method), req->path->data);
    cwist_http_response *res = cwist_http_response_create();
    cwist_http_header_add(&res->headers, "Connection", "close");
    res->keep_alive = false;
    if (!cwist_mux_serve(ctx->router, req, res)) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->status_text, "Not Found");
        cwist_http_header_add(&res->headers, "Content-Type", "application/json");
        cwist_sstring_assign(res->body, "{\"error\":\"not found\"}");
    }
    cwist_http_send_response(client_fd, res);
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    cwist_free(buf);
    close(client_fd);
}

int main() {
    cwist_mux_router *router = cwist_mux_router_create();
    cwist_mux_handle(router, CWIST_HTTP_POST, "/login",   login);
    cwist_mux_handle(router, CWIST_HTTP_GET,  "/profile", profile);

    struct sockaddr_in addr;
    int server_fd = cwist_make_socket_ipv4(&addr, "0.0.0.0", PORT, 128);
    if (server_fd < 0) {
        fprintf(stderr, "Socket error (%d)\n", server_fd);
        cwist_mux_router_destroy(router);
        return 1;
    }

    printf("JWT Auth demo on http://localhost:%d\n", PORT);
    printf("  1. curl -X POST http://localhost:%d/login\n", PORT);
    printf("  2. curl -H 'Authorization: Bearer <token>' http://localhost:%d/profile\n", PORT);
    printf("Ctrl-C to stop.\n");

    ctx_t ctx = { .router = router };
    cwist_server_config cfg = {0};
    cfg.use_threading = true;
    cwist_http_server_loop(server_fd, &cfg, handle_client, &ctx);
    cwist_mux_router_destroy(router);
    return 0;
}
