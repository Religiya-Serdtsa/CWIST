#include <cwist/app.h>
#include <cwist/core/html/css_composer.h>

static void handle_style(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_css_config cfg;
    cwist_css_config_init(&cfg);
    cfg.primary_color = (cwist_color_rgb){0, 102, 204};
    cfg.secondary_color = (cwist_color_rgb){255, 153, 0};
    cfg.is_dark_mode = false;

    cwist_sstring *css = cwist_css_generate_stylesheet(&cfg);
    if (css && css->data) {
        cwist_sstring_assign(res->body, css->data);
        cwist_sstring_destroy(css);
    }
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/style.css", handle_style);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
