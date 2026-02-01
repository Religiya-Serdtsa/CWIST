#ifndef CWIST_HTML_BUILDER_H
#define CWIST_HTML_BUILDER_H

#include <cwist/core/sstring/sstring.h>
#include <cjson/cJSON.h>

typedef struct cwist_html_element {
    cwist_sstring *tag;
    cJSON *attributes;
    struct cwist_html_element **children;
    int child_count;
    cwist_sstring *inner_text;
} cwist_html_element_t;

/** @name Element creation and destruction */
/** @{ */
cwist_html_element_t* cwist_html_element_create(const char *tag);
void cwist_html_element_destroy(cwist_html_element_t *el);
/** @} */

/** @name Attribute manipulation */
/** @{ */
void cwist_html_element_add_attr(cwist_html_element_t *el, const char *key, const char *value);
void cwist_html_element_set_id(cwist_html_element_t *el, const char *id);
void cwist_html_element_add_class(cwist_html_element_t *el, const char *class_name);
/** @} */

/** @name Content and children */
/** @{ */
void cwist_html_element_set_text(cwist_html_element_t *el, const char *text);
void cwist_html_element_add_child(cwist_html_element_t *el, cwist_html_element_t *child);
/** @} */

/** @name Rendering */
/** @{ */
cwist_sstring* cwist_html_render(cwist_html_element_t *el);
/** @} */

#endif
