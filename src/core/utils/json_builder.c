#include <cwist/core/utils/json_builder.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/mem/alloc.h>
#include <stdlib.h>
#include <stdio.h>

/**
 * @file json_builder.c
 * @brief Incremental JSON emission helpers backed by cwist_sstring.
 */

/**
 * @brief Allocate a new JSON builder with an empty output buffer.
 *
 * The builder keeps only a minimal amount of state: the backing string buffer
 * and whether the next value must be prefixed with a comma.
 *
 * @return Newly allocated builder, or NULL when allocation fails.
 */
cwist_json_builder *cwist_json_builder_create(void) {
    cwist_json_builder *b = (cwist_json_builder *)cwist_alloc(sizeof(cwist_json_builder));
    if (!b) return NULL;
    b->buffer = cwist_sstring_create();
    b->needs_comma = false;
    return b;
}

/**
 * @brief Release a builder and its owned string buffer.
 * @param b Builder instance to destroy. NULL is ignored.
 */
void cwist_json_builder_destroy(cwist_json_builder *b) {
    if (b) {
        cwist_sstring_destroy(b->buffer);
        cwist_free(b);
    }
}

/**
 * @brief Insert a comma when the previous operation emitted a complete value.
 * @param b Builder being updated.
 */
static void append_comma_if_needed(cwist_json_builder *b) {
    if (b->needs_comma) {
        cwist_sstring_append(b->buffer, ",");
    }
}

/**
 * @brief Begin a JSON object in the current builder context.
 * @param b Builder to update. NULL is ignored.
 */
void cwist_json_begin_object(cwist_json_builder *b) {
    if (!b) return;
    append_comma_if_needed(b);
    cwist_sstring_append(b->buffer, "{");
    b->needs_comma = false;
}

/**
 * @brief Close the current JSON object.
 * @param b Builder to update. NULL is ignored.
 */
void cwist_json_end_object(cwist_json_builder *b) {
    if (!b) return;
    cwist_sstring_append(b->buffer, "}");
    b->needs_comma = true;
}

/**
 * @brief Begin a JSON array, optionally as the value for an object key.
 * @param b Builder to update. NULL is ignored.
 * @param key Object member name, or NULL when emitting a bare array value.
 */
void cwist_json_begin_array(cwist_json_builder *b, const char *key) {
    if (!b) return;
    append_comma_if_needed(b);
    if (key) {
        cwist_sstring_append(b->buffer, "\"");
        cwist_sstring_append(b->buffer, (char*)key);
        cwist_sstring_append(b->buffer, "\":[");
    } else {
        cwist_sstring_append(b->buffer, "[");
    }
    b->needs_comma = false;
}

/**
 * @brief Close the current JSON array.
 * @param b Builder to update. NULL is ignored.
 */
void cwist_json_end_array(cwist_json_builder *b) {
    if (!b) return;
    cwist_sstring_append(b->buffer, "]");
    b->needs_comma = true;
}

/**
 * @brief Append a JSON string value.
 * @param b Builder to update. NULL is ignored.
 * @param key Object member name, or NULL when appending an array element.
 * @param value String payload to emit verbatim between quotes.
 */
void cwist_json_add_string(cwist_json_builder *b, const char *key, const char *value) {
    if (!b) return;
    append_comma_if_needed(b);
    if (key) {
        cwist_sstring_append(b->buffer, "\"");
        cwist_sstring_append(b->buffer, (char*)key);
        cwist_sstring_append(b->buffer, "\":");
    }
    cwist_sstring_append(b->buffer, "\"");
    cwist_sstring_append(b->buffer, (char*)value);
    cwist_sstring_append(b->buffer, "\"");
    b->needs_comma = true;
}

/**
 * @brief Append a JSON integer value.
 * @param b Builder to update. NULL is ignored.
 * @param key Object member name, or NULL when appending an array element.
 * @param value Integer payload to format in base 10.
 */
void cwist_json_add_int(cwist_json_builder *b, const char *key, int value) {
    if (!b) return;
    append_comma_if_needed(b);
    if (key) {
        cwist_sstring_append(b->buffer, "\"");
        cwist_sstring_append(b->buffer, (char*)key);
        cwist_sstring_append(b->buffer, "\":");
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    cwist_sstring_append(b->buffer, buf);
    b->needs_comma = true;
}

/**
 * @brief Append a JSON boolean literal.
 * @param b Builder to update. NULL is ignored.
 * @param key Object member name, or NULL when appending an array element.
 * @param value Boolean payload to serialise as true or false.
 */
void cwist_json_add_bool(cwist_json_builder *b, const char *key, bool value) {
    if (!b) return;
    append_comma_if_needed(b);
    if (key) {
        cwist_sstring_append(b->buffer, "\"");
        cwist_sstring_append(b->buffer, (char*)key);
        cwist_sstring_append(b->buffer, "\":");
    }
    cwist_sstring_append(b->buffer, value ? "true" : "false");
    b->needs_comma = true;
}

/**
 * @brief Append a JSON null literal.
 * @param b Builder to update. NULL is ignored.
 * @param key Object member name, or NULL when appending an array element.
 */
void cwist_json_add_null(cwist_json_builder *b, const char *key) {
    if (!b) return;
    append_comma_if_needed(b);
    if (key) {
        cwist_sstring_append(b->buffer, "\"");
        cwist_sstring_append(b->buffer, (char*)key);
        cwist_sstring_append(b->buffer, "\":");
    }
    cwist_sstring_append(b->buffer, "null");
    b->needs_comma = true;
}

/**
 * @brief Expose the builder's raw character buffer.
 * @param b Builder whose buffer should be observed.
 * @return Null-terminated JSON text, or NULL when the builder is invalid.
 */
const char *cwist_json_get_raw(cwist_json_builder *b) {
    if (!b || !b->buffer) return NULL;
    return b->buffer->data;
}
