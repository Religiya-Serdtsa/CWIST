/**
 * @file main.c
 * @brief 17-blog-crud — full blog combining all micro-examples.
 *
 *   SQLite + ORM  |  JSON API  |  HTML builder + CSS composer
 *   Path params   |  Query params  |  JWT auth  |  Middleware
 *   Static files  |  PQC TLS  |  Metrics  |  Healthz
 */

#include <cwist/app.h>
#include <cwist/core/orm/orm.h>
#include <cwist/core/orm/orm_socket.h>
#include <cwist/core/html/builder.h>
#include <cwist/core/html/css_composer.h>
#include <cwist/core/utils/json_builder.h>
#include <cwist/security/jwt/jwt.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/net/http/query.h>
#include <string.h>
#include <stdio.h>

#define JWT_SECRET "blog-secret"

static cwist_orm_t *g_orm = NULL;

/* ---------- Middleware ---------- */
static void logger(cwist_http_request *req, cwist_http_response *res,
                   cwist_handler_func next) {
    (void)res;
    printf("[LOG] %s %s\n", cwist_http_method_to_string(req->method),
           req->path->data);
    next(req, res);
}

static void require_auth(cwist_http_request *req, cwist_http_response *res,
                         cwist_handler_func next) {
    char *auth = cwist_http_header_get(req->headers, "Authorization");
    if (!auth || strncmp(auth, "Bearer ", 7) != 0) {
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->body, "{\"error\":\"unauthorized\"}");
        return;
    }
    cwist_jwt_claims *claims = cwist_jwt_verify(auth + 7, JWT_SECRET);
    if (!claims) {
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->body, "{\"error\":\"invalid token\"}");
        return;
    }
    cwist_jwt_claims_destroy(claims);
    next(req, res);
}

/* ---------- HTML UI ---------- */
static cwist_sstring *form_ui(void) {
    cwist_css_config cfg;
    cwist_css_config_init(&cfg);
    cwist_sstring *css = cwist_css_generate_stylesheet(&cfg);

    cwist_html_element_t *html = cwist_html_element_create("html");
    cwist_html_element_t *head = cwist_html_element_create("head");
    cwist_html_element_t *style = cwist_html_element_create("style");
    cwist_html_element_set_text(style, css->data);
    cwist_html_element_add_child(head, style);
    cwist_html_element_add_child(html, head);

    cwist_html_element_t *body = cwist_html_element_create("body");
    cwist_html_element_t *h1 = cwist_html_element_create("h1");
    cwist_html_element_set_text(h1, "CWIST Blog");
    cwist_html_element_add_child(body, h1);

    cJSON *rows = NULL;
    cwist_orm_select(g_orm, "posts", "id, title", NULL, &rows);
    if (rows) {
        int n = cJSON_GetArraySize(rows);
        cwist_html_element_t *ul = cwist_html_element_create("ul");
        for (int i = 0; i < n; i++) {
            cJSON *row = cJSON_GetArrayItem(rows, i);
            cJSON *id = cJSON_GetObjectItem(row, "id");
            cJSON *title = cJSON_GetObjectItem(row, "title");

            cwist_html_element_t *li = cwist_html_element_create("li");
            cwist_html_element_t *a = cwist_html_element_create("a");
            char href[64];
            snprintf(href, sizeof(href), "/posts/%s",
                     id ? id->valuestring : "0");
            cwist_html_element_add_attr(a, "href", href);
            cwist_html_element_set_text(a, title ? title->valuestring : "Untitled");
            cwist_html_element_add_child(li, a);
            cwist_html_element_add_child(ul, li);
        }
        cwist_html_element_add_child(body, ul);
        cJSON_Delete(rows);
    }

    cwist_html_element_add_child(html, body);
    cwist_sstring *out = cwist_html_render(html);
    cwist_html_element_destroy(html);
    cwist_sstring_destroy(css);
    return out;
}

static void index_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring *html = form_ui();
    cwist_http_header_add(&res->headers, "Content-Type", "text/html");
    cwist_sstring_assign(res->body, html->data);
    cwist_sstring_destroy(html);
}

/* ---------- JSON API ---------- */
static void api_list(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cJSON *rows = NULL;
    cwist_orm_select(g_orm, "posts", "*", NULL, &rows);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddItemToObject(out, "posts", rows ? rows : cJSON_CreateArray());
    char *json = cJSON_Print(out);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, json);
    free(json);
    cJSON_Delete(out);
}

static void api_show(cwist_http_request *req, cwist_http_response *res) {
    const char *id = cwist_query_map_get(req->path_params, "id");
    char where[64];
    snprintf(where, sizeof(where), "id = %s", id ? id : "0");

    cJSON *rows = NULL;
    cwist_orm_select(g_orm, "posts", "*", where, &rows);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddItemToObject(out, "post",
        (rows && cJSON_GetArraySize(rows) > 0)
            ? cJSON_Duplicate(cJSON_GetArrayItem(rows, 0), true)
            : cJSON_CreateObject());
    char *json = cJSON_Print(out);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, json);
    free(json);
    cJSON_Delete(out);
    if (rows) cJSON_Delete(rows);
}

static void api_create(cwist_http_request *req, cwist_http_response *res) {
    cJSON *in = cJSON_Parse(req->body->data);
    if (!in) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "{\"error\":\"bad json\"}");
        return;
    }
    cwist_orm_insert(g_orm, "posts", in);
    cJSON_Delete(in);
    cwist_sstring_assign(res->body, "{\"status\":\"created\"}");
}

static void api_update(cwist_http_request *req, cwist_http_response *res) {
    const char *id = cwist_query_map_get(req->path_params, "id");
    cJSON *in = cJSON_Parse(req->body->data);
    if (!in || !id) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "{\"error\":\"bad request\"}");
        return;
    }
    char where[64];
    snprintf(where, sizeof(where), "id = %s", id);
    cwist_orm_update(g_orm, "posts", in, where);
    cJSON_Delete(in);
    cwist_sstring_assign(res->body, "{\"status\":\"updated\"}");
}

static void api_delete(cwist_http_request *req, cwist_http_response *res) {
    const char *id = cwist_query_map_get(req->path_params, "id");
    if (!id) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "{\"error\":\"missing id\"}");
        return;
    }
    char where[64];
    snprintf(where, sizeof(where), "id = %s", id);
    cwist_orm_delete(g_orm, "posts", where);
    cwist_sstring_assign(res->body, "{\"status\":\"deleted\"}");
}

static void login(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    char *token = cwist_jwt_sign("{\"sub\":\"1\",\"role\":\"admin\"}",
                                 JWT_SECRET, 3600);
    cwist_json_builder *jb = cwist_json_builder_create();
    cwist_json_begin_object(jb);
    cwist_json_add_string(jb, "token", token ? token : "");
    cwist_json_end_object(jb);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, (char *)cwist_json_get_raw(jb));
    cwist_json_builder_destroy(jb);
    cwist_free(token);
}

/* ---------- Main ---------- */
int main(void) {
    int sock = cwist_db_transfer_sqlite_to_socket(":memory:");
    g_orm = cwist_orm_open_socket(sock);
    cwist_orm_immediate_commit(true);
    cwist_orm_exec(g_orm,
        "CREATE TABLE posts(id INTEGER PRIMARY KEY, title TEXT, body TEXT);");

    cwist_app *app = cwist_app_create();
    cwist_app_use(app, logger);
    cwist_app_use_pqc_layer(app, true);
    cwist_app_enable_metrics(app);
    cwist_app_enable_healthz(app);
    cwist_app_static(app, "/static", "public");

    /* Frontend */
    cwist_app_get(app, "/", index_handler);

    /* API */
    cwist_app_get(app,  "/api/posts",          api_list);
    cwist_app_get(app,  "/api/posts/:id",      api_show);
    cwist_app_post(app, "/api/posts",          api_create);
    cwist_app_post(app, "/api/posts/:id/edit", api_update);
    cwist_app_post(app, "/api/posts/:id/delete", api_delete);

    /* Auth */
    cwist_app_post(app, "/login", login);

    cwist_app_use_https(app, "server.crt", "server.key");
    printf("Blog server on https://localhost:8443\n");
    cwist_app_listen(app, 8443);

    cwist_orm_close_socket(g_orm);
    cwist_app_destroy(app);
    return 0;
}
