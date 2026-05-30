/**
 * @file main.c
 * @brief Express-style routing without manual mux glue.
 *
 * HTML markup is produced by form_ui() using the HTML builder and dynamic
 * CSS composer.  Handlers only set the rendered body on the response.
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
    cwist_html_element_set_text(h1, "CWIST Router");
    cwist_html_element_add_child(body, h1);

    cwist_html_element_t *ul = cwist_html_element_create("ul");

    cwist_html_element_t *li1 = cwist_html_element_create("li");
    cwist_html_element_t *a1 = cwist_html_element_create("a");
    cwist_html_element_add_attr(a1, "href", "/hello");
    cwist_html_element_set_text(a1, "GET /hello");
    cwist_html_element_add_child(li1, a1);
    cwist_html_element_add_child(ul, li1);

    cwist_html_element_t *li2 = cwist_html_element_create("li");
    cwist_html_element_set_text(li2, "POST /echo (send any body)");
    cwist_html_element_add_child(ul, li2);

    cwist_html_element_add_child(body, ul);
    cwist_html_element_add_child(html, body);

    cwist_sstring *out = cwist_html_render(html);
    cwist_html_element_destroy(html);
    cwist_sstring_destroy(css);
    return out;
}

static void home(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring *html = form_ui();
    cwist_http_header_add(&res->headers, "Content-Type", "text/html");
    cwist_sstring_assign(res->body, html->data);
    cwist_sstring_destroy(html);
}

static void hello(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "Hello from the router!");
}

static void echo(cwist_http_request *req, cwist_http_response *res) {
    cwist_sstring_assign(res->body, req->body ? req->body->data : "");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/", home);
    cwist_app_get(app, "/hello", hello);
    cwist_app_post(app, "/echo", echo);
    cwist_app_listen(app, 8081);
    cwist_app_destroy(app);
    return 0;
}
