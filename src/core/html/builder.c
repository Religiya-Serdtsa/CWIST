#include <cwist/core/html/builder.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/mem/alloc.h>
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/**
 * @file builder.c
 * @brief Minimal HTML tree construction and rendering helpers.
 */

/**
 * @brief Allocate a new HTML element node for the supplied tag name.
 * @param tag Tag name to copy into the node, such as "div" or "span".
 * @return Newly allocated element, or NULL when allocation fails.
 */
cwist_html_element_t* cwist_html_element_create(const char *tag) {
    cwist_html_element_t *el = (cwist_html_element_t *)cwist_alloc(sizeof(cwist_html_element_t));
    if (!el) return NULL;
    
    el->tag = cwist_sstring_create();
    cwist_sstring_assign(el->tag, (char*)tag);
    el->attributes = cJSON_CreateObject();
    el->children = NULL;
    el->child_count = 0;
    el->inner_text = NULL;
    
    return el;
}

/**
 * @brief Recursively destroy an element and every descendant it owns.
 * @param el Element tree root to release. NULL is ignored.
 */
void cwist_html_element_destroy(cwist_html_element_t *el) {
    if (!el) return;
    
    if (el->tag) cwist_sstring_destroy(el->tag);
    if (el->attributes) cJSON_Delete(el->attributes);
    
    if (el->children) {
        for (int i = 0; i < el->child_count; i++) {
            cwist_html_element_destroy(el->children[i]);
        }
        cwist_free(el->children);
    }
    
    if (el->inner_text) cwist_sstring_destroy(el->inner_text);
    
    cwist_free(el);
}

/**
 * @brief Insert or replace an HTML attribute on an element.
 * @param el Element to update.
 * @param key Attribute name, for example "class".
 * @param value Attribute value stored as a cJSON string node.
 */
void cwist_html_element_add_attr(cwist_html_element_t *el, const char *key, const char *value) {
    if (!el || !key || !value) return;
    
    if (cJSON_HasObjectItem(el->attributes, key)) {
        cJSON_ReplaceItemInObject(el->attributes, key, cJSON_CreateString(value));
    } else {
        cJSON_AddStringToObject(el->attributes, key, value);
    }
}

/**
 * @brief Set the DOM id attribute for an element.
 * @param el Element to update.
 * @param id Identifier value to assign.
 */
void cwist_html_element_set_id(cwist_html_element_t *el, const char *id) {
    cwist_html_element_add_attr(el, "id", id);
}

/**
 * @brief Append a CSS class token to an element.
 * @param el Element to update.
 * @param class_name Class token to append or initialise.
 */
void cwist_html_element_add_class(cwist_html_element_t *el, const char *class_name) {
    if (!el || !class_name) return;
    
    cJSON *cls = cJSON_GetObjectItem(el->attributes, "class");
    if (cls) {
        // Append to existing class
        cwist_sstring *s = cwist_sstring_create();
        cwist_sstring_assign(s, cls->valuestring);
        cwist_sstring_append(s, " ");
        cwist_sstring_append(s, class_name);
        cJSON_ReplaceItemInObject(el->attributes, "class", cJSON_CreateString(s->data));
        cwist_sstring_destroy(s);
    } else {
        cwist_html_element_add_attr(el, "class", class_name);
    }
}

/**
 * @brief Replace the element's inner text payload.
 * @param el Element to update.
 * @param text Plain-text payload to store.
 */
void cwist_html_element_set_text(cwist_html_element_t *el, const char *text) {
    if (!el) return;
    if (el->inner_text) {
        cwist_sstring_assign(el->inner_text, (char*)text);
    } else {
        el->inner_text = cwist_sstring_create();
        cwist_sstring_assign(el->inner_text, (char*)text);
    }
}

/**
 * @brief Append a child node to an element's ordered children list.
 * @param el Parent element that will own the child.
 * @param child Child node to append.
 */
void cwist_html_element_add_child(cwist_html_element_t *el, cwist_html_element_t *child) {
    if (!el || !child) return;
    
    el->child_count++;
    cwist_html_element_t **new_children = (cwist_html_element_t **)cwist_realloc(el->children, sizeof(cwist_html_element_t*) * el->child_count);
    if (!new_children) {
        el->child_count--;
        return; 
    }
    el->children = new_children;
    el->children[el->child_count - 1] = child;
}

/**
 * @brief Serialise a node and its descendants into an output buffer.
 * @param el Current element being rendered.
 * @param out Destination buffer that receives generated markup.
 */
static void render_element(cwist_html_element_t *el, cwist_sstring *out) {
    if (!el) return;
    
    if(el->tag) {
        cwist_sstring_append(out, "<");
        cwist_sstring_append(out, el->tag->data);
        cJSON *attr = NULL;
        cJSON_ArrayForEach(attr, el->attributes) {
            cwist_sstring_append(out, " ");
            cwist_sstring_append(out, attr->string);
            cwist_sstring_append(out, "=\"");
            cwist_sstring_append(out, attr->valuestring);
            cwist_sstring_append(out, "\"");
        }
        cwist_sstring_append(out, ">");
    } else if(el->inner_text) {
        cwist_sstring_append_escaped(out, el->inner_text->data);
    }
    
    if (el->inner_text) {
        cwist_sstring_append(out, el->inner_text->data);
    }
    
    if (el->children) {
        for (int i = 0; i < el->child_count; i++) {
            render_element(el->children[i], out);
        }
    }
    
    cwist_sstring_append(out, "</");
    cwist_sstring_append(out, el->tag->data);
    cwist_sstring_append(out, ">");
}

/**
 * @brief Render an HTML tree to a freshly allocated string buffer.
 * @param el Root element to serialise.
 * @return Rendered HTML buffer, or NULL when the root is invalid.
 */
cwist_sstring* cwist_html_render(cwist_html_element_t *el) {
    if (!el) return NULL;
    cwist_sstring *out = cwist_sstring_create();
    cwist_sstring_assign(out, "");
    render_element(el, out);
    return out;
}
