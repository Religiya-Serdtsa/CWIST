#include <cwist/app.h>
#include <cwist/core/html/builder.h>

static void handle_html(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_html_element_t *html = cwist_html_element_create("html");
    cwist_html_element_t *body = cwist_html_element_create("body");
    cwist_html_element_t *h1 = cwist_html_element_create("h1");
    cwist_html_element_set_text(h1, "Programmatic HTML Builder");
    cwist_html_element_add_class(h1, "title");

    cwist_html_element_add_child(body, h1);
    cwist_html_element_add_child(html, body);

    cwist_sstring *rendered = cwist_html_render(html);
    if (rendered && rendered->data) {
        cwist_sstring_assign(res->body, rendered->data);
        cwist_sstring_destroy(rendered);
    }
    cwist_html_element_destroy(html);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/", handle_html);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
