#include <stdio.h>
#include <cwist/core/html/builder.h>

int main() {
    printf("=== HTML Builder Tutorial ===\n");

    /* ---- 1. Simple paragraph ---- */
    printf("\n[Simple paragraph]\n");
    cwist_html_element_t *p = cwist_html_element_create("p");
    cwist_html_element_set_id(p, "intro");
    cwist_html_element_add_class(p, "text-lg");
    cwist_html_element_set_text(p, "Hello from CWIST HTML Builder!");

    cwist_sstring *html = cwist_html_render(p);
    if (html) {
        printf("%s\n", html->data);
        cwist_sstring_destroy(html);
    }
    cwist_html_element_destroy(p);

    /* ---- 2. Nested elements ---- */
    printf("\n[Nested: div > ul > li*3]\n");
    cwist_html_element_t *div = cwist_html_element_create("div");
    cwist_html_element_add_class(div, "container");

    cwist_html_element_t *ul = cwist_html_element_create("ul");

    const char *items[] = {"Alpha", "Beta", "Gamma"};
    for (int i = 0; i < 3; i++) {
        cwist_html_element_t *li = cwist_html_element_create("li");
        cwist_html_element_set_text(li, items[i]);
        cwist_html_element_add_child(ul, li);
    }

    cwist_html_element_add_child(div, ul);

    html = cwist_html_render(div);
    if (html) {
        printf("%s\n", html->data);
        cwist_sstring_destroy(html);
    }
    cwist_html_element_destroy(div);

    /* ---- 3. Link with custom attribute ---- */
    printf("\n[Anchor with href]\n");
    cwist_html_element_t *a = cwist_html_element_create("a");
    cwist_html_element_add_attr(a, "href", "https://example.com");
    cwist_html_element_add_attr(a, "target", "_blank");
    cwist_html_element_set_text(a, "Visit Example.com");

    html = cwist_html_render(a);
    if (html) {
        printf("%s\n", html->data);
        cwist_sstring_destroy(html);
    }
    cwist_html_element_destroy(a);

    printf("\n=== Done ===\n");
    return 0;
}
