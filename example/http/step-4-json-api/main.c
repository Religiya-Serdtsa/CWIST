#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/mux.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/utils/json_builder.h>
#include <cwist/core/mem/alloc.h>
#include <cjson/cJSON.h>

#define PORT 8083

/* GET /users — return a JSON array of mock users */
static void get_users(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_json_builder *jb = cwist_json_builder_create();
    cwist_json_begin_array(jb, NULL);

    /* User 1 */
    cwist_sstring_append(jb->buffer, "{");
    jb->needs_comma = false;
    cwist_json_add_int(jb, "id", 1);
    cwist_json_add_string(jb, "name", "Alice");
    cwist_json_add_bool(jb, "active", true);
    cwist_sstring_append(jb->buffer, "}");

    cwist_sstring_append(jb->buffer, ",{");
    jb->needs_comma = false;
    cwist_json_add_int(jb, "id", 2);
    cwist_json_add_string(jb, "name", "Bob");
    cwist_json_add_bool(jb, "active", false);
    cwist_sstring_append(jb->buffer, "}");

    cwist_json_end_array(jb);

    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->status_text, "OK");
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

    res->status_code = CWIST_HTTP_CREATED;
    cwist_sstring_assign(res->status_text, "Created");
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
    cwist_mux_handle(router, CWIST_HTTP_GET,  "/users", get_users);
    cwist_mux_handle(router, CWIST_HTTP_POST, "/users", create_user);

    struct sockaddr_in addr;
    int server_fd = cwist_make_socket_ipv4(&addr, "0.0.0.0", PORT, 128);
    if (server_fd < 0) {
        fprintf(stderr, "Failed to create socket (err=%d)\n", server_fd);
        cwist_mux_router_destroy(router);
        return 1;
    }

    printf("JSON API on http://localhost:%d\n", PORT);
    printf("  GET  /users\n");
    printf("  POST /users  (body: {\"name\":\"Carol\"})\n");
    printf("Ctrl-C to stop.\n");

    ctx_t ctx = { .router = router };
    cwist_server_config cfg = {0};
    cfg.use_threading = true;
    cwist_http_server_loop(server_fd, &cfg, handle_client, &ctx);

    cwist_mux_router_destroy(router);
    return 0;
}
