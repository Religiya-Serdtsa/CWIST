/**
 * @file main.c
 * @brief CWIST simple server using the high-level app API.
 *
 * HTML is composed dynamically via cwist_html_builder and cwist_css_composer
 * inside form_ui().  Handlers receive the rendered markup — no hard-coded
 * strings, no manual socket code.
 */

#include <cwist/app.h>
#include <cwist/core/html/builder.h>
#include <cwist/core/html/css_composer.h>

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
    cwist_html_element_set_text(h1, "Hello from CWIST!");
    cwist_html_element_add_child(body, h1);

    cwist_html_element_t *p = cwist_html_element_create("p");
    cwist_html_element_set_text(p, "A high-performance C17 web framework.");
    cwist_html_element_add_child(body, p);

    cwist_html_element_t *nav = cwist_html_element_create("nav");

    cwist_html_element_t *a1 = cwist_html_element_create("a");
    cwist_html_element_add_attr(a1, "href", "/health");
    cwist_html_element_set_text(a1, "Health");
    cwist_html_element_add_child(nav, a1);

    cwist_html_element_t *span = cwist_html_element_create("span");
    cwist_html_element_set_text(span, " | ");
    cwist_html_element_add_child(nav, span);

    cwist_html_element_t *a2 = cwist_html_element_create("a");
    cwist_html_element_add_attr(a2, "href", "/json");
    cwist_html_element_set_text(a2, "JSON");
    cwist_html_element_add_child(nav, a2);

    cwist_html_element_add_child(body, nav);
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

static void health_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, "{\"status\":\"ok\",\"uptime\":\"forever\"}");
}

static void echo_handler(cwist_http_request *req, cwist_http_response *res) {
    char *ct = cwist_http_header_get(req->headers, "Content-Type");
    if (ct) cwist_http_header_add(&res->headers, "Content-Type", ct);
    cwist_sstring_assign(res->body, req->body->data);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/", index_handler);
    cwist_app_get(app, "/health", health_handler);
    cwist_app_post(app, "/echo", echo_handler);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
