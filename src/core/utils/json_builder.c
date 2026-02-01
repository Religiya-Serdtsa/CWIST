#include <cwist/core/utils/json_builder.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/mem/alloc.h>
#include <stdlib.h>
#include <stdio.h>

cwist_json_builder *cwist_json_builder_create(void) {
    cwist_json_builder *b = (cwist_json_builder *)cwist_alloc(sizeof(cwist_json_builder));
    if (!b) return NULL;
    b->buffer = cwist_sstring_create();
    b->needs_comma = false;
    return b;
}

void cwist_json_builder_destroy(cwist_json_builder *b) {
    if (b) {
        cwist_sstring_destroy(b->buffer);
        cwist_free(b);
    }
}

static void append_comma_if_needed(cwist_json_builder *b) {
    if (b->needs_comma) {
        cwist_sstring_append(b->buffer, ",");
    }
}

void cwist_json_begin_object(cwist_json_builder *b) {
    if (!b) return;
    append_comma_if_needed(b);
    cwist_sstring_append(b->buffer, "{");
    b->needs_comma = false;
}

void cwist_json_end_object(cwist_json_builder *b) {
    if (!b) return;
    cwist_sstring_append(b->buffer, "}");
    b->needs_comma = true;
}

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

void cwist_json_end_array(cwist_json_builder *b) {
    if (!b) return;
    cwist_sstring_append(b->buffer, "]");
    b->needs_comma = true;
}

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

const char *cwist_json_get_raw(cwist_json_builder *b) {
    if (!b || !b->buffer) return NULL;
    return b->buffer->data;
}
