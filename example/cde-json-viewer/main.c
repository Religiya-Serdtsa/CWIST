/**
 * @file main.c
 * @brief CDE-style JSON viewer using the app-level API with HTML builder
 *        and dynamic CSS composer.
 */

#include <cwist/app.h>
#include <cwist/core/html/builder.h>
#include <cwist/core/html/css_composer.h>
#include <cjson/cJSON.h>

static const char *MOCK_JSON =
    "{\"System\":\"Solaris 2.5.1\",\"Host\":\"sun-sparc-station\","
    "\"User\":\"yjlee\",\"Shell\":\"/bin/csh\","
    "\"Uptime\":\"42 days\",\"Load\":\"0.01, 0.05, 0.00\"}";

static cwist_sstring *form_ui(cJSON *json) {
    cwist_css_config cfg;
    cwist_css_config_init(&cfg);
    cfg.primary_color   = cwist_color_hex_to_rgb("#4b6983");
    cfg.secondary_color = cwist_color_hex_to_rgb("#5d97a6");
    cwist_sstring *css  = cwist_css_generate_stylesheet(&cfg);

    cwist_html_element_t *html = cwist_html_element_create("html");
    cwist_html_element_t *head = cwist_html_element_create("head");
    cwist_html_element_t *style = cwist_html_element_create("style");
    cwist_html_element_set_text(style, css->data);
    cwist_html_element_add_child(head, style);
    cwist_html_element_add_child(html, head);

    cwist_html_element_t *body = cwist_html_element_create("body");
    cwist_html_element_t *win  = cwist_html_element_create("div");
    cwist_html_element_add_class(win, "container");

    cwist_html_element_t *title = cwist_html_element_create("h2");
    cwist_html_element_set_text(title, "System Monitor");
    cwist_html_element_add_child(win, title);

    cwist_html_element_t *table = cwist_html_element_create("table");
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, json) {
        if (cJSON_IsString(item)) {
            cwist_html_element_t *tr = cwist_html_element_create("tr");

            cwist_html_element_t *td_key = cwist_html_element_create("td");
            cwist_html_element_set_text(td_key, item->string);
            cwist_html_element_add_child(tr, td_key);

            cwist_html_element_t *td_val = cwist_html_element_create("td");
            cwist_html_element_set_text(td_val, item->valuestring);
            cwist_html_element_add_child(tr, td_val);

            cwist_html_element_add_child(table, tr);
        }
    }
    cwist_html_element_add_child(win, table);
    cwist_html_element_add_child(body, win);
    cwist_html_element_add_child(html, body);

    cwist_sstring *out = cwist_html_render(html);
    cwist_html_element_destroy(html);
    cwist_sstring_destroy(css);
    return out;
}

static void index_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cJSON *json = cJSON_Parse(MOCK_JSON);
    if (!json) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }
    cwist_sstring *html = form_ui(json);
    cJSON_Delete(json);
    cwist_http_header_add(&res->headers, "Content-Type", "text/html");
    cwist_sstring_assign(res->body, html->data);
    cwist_sstring_destroy(html);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/", index_handler);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
