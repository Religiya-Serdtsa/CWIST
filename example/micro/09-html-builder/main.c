/**
 * @file main.c
 * @brief 09-html-builder — dynamic HTML + CSS.
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
    cwist_html_element_set_text(h1, "CWIST Blog");
    cwist_html_element_add_child(body, h1);
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

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/", index_handler);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
