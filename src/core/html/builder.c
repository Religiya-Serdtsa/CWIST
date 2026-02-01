#include <cwist/core/html/builder.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/mem/alloc.h>
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

void cwist_html_element_add_attr(cwist_html_element_t *el, const char *key, const char *value) {
    if (!el || !key || !value) return;
    
    if (cJSON_HasObjectItem(el->attributes, key)) {
        cJSON_ReplaceItemInObject(el->attributes, key, cJSON_CreateString(value));
    } else {
        cJSON_AddStringToObject(el->attributes, key, value);
    }
}

void cwist_html_element_set_id(cwist_html_element_t *el, const char *id) {
    cwist_html_element_add_attr(el, "id", id);
}

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

void cwist_html_element_set_text(cwist_html_element_t *el, const char *text) {
    if (!el) return;
    if (el->inner_text) {
        cwist_sstring_assign(el->inner_text, (char*)text);
    } else {
        el->inner_text = cwist_sstring_create();
        cwist_sstring_assign(el->inner_text, (char*)text);
    }
}

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

static void render_element(cwist_html_element_t *el, cwist_sstring *out) {
    if (!el) return;
    
    if(el->tag) {
        cwist_sstring_append(out, "<");
        cwist_sstring_append(out, el->tag->data);
        // Render attributes (Use normal append for system-generated attrs)
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
    
    // Render attributes
    cJSON *attr = NULL;
    cJSON_ArrayForEach(attr, el->attributes) {
        cwist_sstring_append(out, " ");
        cwist_sstring_append(out, attr->string);
        cwist_sstring_append(out, "=\" ");
        cwist_sstring_append(out, attr->valuestring);
        cwist_sstring_append(out, "\"");
    }
    
    cwist_sstring_append(out, ">");
    
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

cwist_sstring* cwist_html_render(cwist_html_element_t *el) {
    if (!el) return NULL;
    cwist_sstring *out = cwist_sstring_create();
    cwist_sstring_assign(out, "");
    render_element(el, out);
    return out;
}
