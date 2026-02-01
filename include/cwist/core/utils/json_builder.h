#ifndef __CWIST_JSON_BUILDER_H__
#define __CWIST_JSON_BUILDER_H__

#include <cwist/core/sstring/sstring.h>
#include <stdbool.h>

/**
 * @brief Simple JSON string builder utility.
 *
 * Usage:
 * @code
 * cwist_json_builder *jb = cwist_json_builder_create();
 * cwist_json_begin_object(jb);
 * cwist_json_add_string(jb, "message", "Hello");
 * cwist_json_add_int(jb, "code", 200);
 * cwist_json_end_object(jb);
 * char *json = cwist_json_build(jb);
 * cwist_sstring *res = cwist_json_get_string(jb);
 * cwist_json_builder_destroy(jb);
 * @endcode
 */

typedef struct cwist_json_builder {
    cwist_sstring *buffer;
    bool needs_comma;
} cwist_json_builder;

cwist_json_builder *cwist_json_builder_create(void);
void cwist_json_builder_destroy(cwist_json_builder *b);

void cwist_json_begin_object(cwist_json_builder *b);
void cwist_json_end_object(cwist_json_builder *b);
void cwist_json_begin_array(cwist_json_builder *b, const char *key);
void cwist_json_end_array(cwist_json_builder *b);

void cwist_json_add_string(cwist_json_builder *b, const char *key, const char *value);
void cwist_json_add_int(cwist_json_builder *b, const char *key, int value);
void cwist_json_add_bool(cwist_json_builder *b, const char *key, bool value);
void cwist_json_add_null(cwist_json_builder *b, const char *key);

/** @brief Returns the raw string buffer. Invalidated on destroy. */
const char *cwist_json_get_raw(cwist_json_builder *b);

#endif
