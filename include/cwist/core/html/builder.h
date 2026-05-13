/**
 * @file builder.h
 * @brief HTML element builder and renderer.
 */

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

/**
 * @brief Create a new HTML element.
 */
cwist_html_element_t* cwist_html_element_create(const char *tag);

/**
 * @brief Destroy an HTML element and its children.
 */
void cwist_html_element_destroy(cwist_html_element_t *el);

/** @} */

/** @name Attribute manipulation */
/** @{ */

/**
 * @brief Add an attribute to an element.
 */
void cwist_html_element_add_attr(cwist_html_element_t *el, const char *key, const char *value);

/**
 * @brief Set the id attribute.
 */
void cwist_html_element_set_id(cwist_html_element_t *el, const char *id);

/**
 * @brief Add a CSS class.
 */
void cwist_html_element_add_class(cwist_html_element_t *el, const char *class_name);

/** @} */

/** @name Content and children */
/** @{ */

/**
 * @brief Set inner text.
 */
void cwist_html_element_set_text(cwist_html_element_t *el, const char *text);

/**
 * @brief Add a child element.
 */
void cwist_html_element_add_child(cwist_html_element_t *el, cwist_html_element_t *child);

/** @} */

/** @name Rendering */
/** @{ */

/**
 * @brief Render the element tree to an HTML string.
 */
cwist_sstring* cwist_html_render(cwist_html_element_t *el);

/** @} */

#endif
