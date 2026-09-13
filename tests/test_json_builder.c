/**
 * @file test_json_builder.c
 * @brief Unit tests for JSON builder string escaping, NULL guards, and formatting.
 */

#include <cwist/core/utils/json_builder.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

static void test_json_builder_null_guards(void) {
    printf("test_json_builder_null_guards...\n");
    cwist_json_builder_destroy(NULL);
    cwist_json_begin_object(NULL);
    cwist_json_end_object(NULL);
    cwist_json_begin_array(NULL, "arr");
    cwist_json_end_array(NULL);
    cwist_json_add_string(NULL, "k", "v");
    cwist_json_add_int(NULL, "k", 10);
    cwist_json_add_bool(NULL, "k", true);
    cwist_json_add_null(NULL, "k");
    assert(cwist_json_get_raw(NULL) == NULL);
}

static void test_json_builder_basic(void) {
    printf("test_json_builder_basic...\n");
    cwist_json_builder *b = cwist_json_builder_create();
    assert(b != NULL);

    cwist_json_begin_object(b);
    cwist_json_add_string(b, "name", "Alice");
    cwist_json_add_int(b, "age", 30);
    cwist_json_add_bool(b, "active", true);
    cwist_json_add_null(b, "nickname");
    cwist_json_end_object(b);

    const char *raw = cwist_json_get_raw(b);
    assert(raw != NULL);
    assert(strcmp(raw, "{\"name\":\"Alice\",\"age\":30,\"active\":true,\"nickname\":null}") == 0);

    cJSON *parsed = cJSON_Parse(raw);
    assert(parsed != NULL);
    assert(cJSON_GetObjectItem(parsed, "name") != NULL);
    assert(strcmp(cJSON_GetObjectItem(parsed, "name")->valuestring, "Alice") == 0);
    assert(cJSON_GetObjectItem(parsed, "age")->valueint == 30);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(parsed, "active")));
    assert(cJSON_IsNull(cJSON_GetObjectItem(parsed, "nickname")));
    cJSON_Delete(parsed);

    cwist_json_builder_destroy(b);
}

static void test_json_builder_escaping(void) {
    printf("test_json_builder_escaping...\n");
    cwist_json_builder *b = cwist_json_builder_create();
    assert(b != NULL);

    cwist_json_begin_object(b);
    cwist_json_add_string(b, "quote_and_slash", "He said \"hello\" \\ world");
    cwist_json_add_string(b, "whitespace", "Line1\nLine2\r\tTabbed\b\f");
    char ctrl_str[4] = {0x01, 0x1f, 0, 0};
    cwist_json_add_string(b, "control", ctrl_str);
    cwist_json_end_object(b);

    const char *raw = cwist_json_get_raw(b);
    assert(raw != NULL);

    cJSON *parsed = cJSON_Parse(raw);
    assert(parsed != NULL);
    assert(strcmp(cJSON_GetObjectItem(parsed, "quote_and_slash")->valuestring, "He said \"hello\" \\ world") == 0);
    assert(strcmp(cJSON_GetObjectItem(parsed, "whitespace")->valuestring, "Line1\nLine2\r\tTabbed\b\f") == 0);
    assert(cJSON_GetObjectItem(parsed, "control")->valuestring[0] == 0x01);
    assert((unsigned char)cJSON_GetObjectItem(parsed, "control")->valuestring[1] == 0x1f);
    cJSON_Delete(parsed);

    cwist_json_builder_destroy(b);
}

static void test_json_builder_array_and_null_value(void) {
    printf("test_json_builder_array_and_null_value...\n");
    cwist_json_builder *b = cwist_json_builder_create();
    assert(b != NULL);

    cwist_json_begin_object(b);
    cwist_json_add_string(b, "null_str", NULL);
    cwist_json_begin_array(b, "items");
    cwist_json_add_string(b, NULL, "first");
    cwist_json_add_int(b, NULL, 42);
    cwist_json_add_bool(b, NULL, false);
    cwist_json_add_null(b, NULL);
    cwist_json_end_array(b);
    cwist_json_end_object(b);

    const char *raw = cwist_json_get_raw(b);
    assert(raw != NULL);
    assert(strcmp(raw, "{\"null_str\":null,\"items\":[\"first\",42,false,null]}") == 0);

    cJSON *parsed = cJSON_Parse(raw);
    assert(parsed != NULL);
    assert(cJSON_IsNull(cJSON_GetObjectItem(parsed, "null_str")));
    cJSON *items = cJSON_GetObjectItem(parsed, "items");
    assert(cJSON_IsArray(items));
    assert(cJSON_GetArraySize(items) == 4);
    cJSON_Delete(parsed);

    cwist_json_builder_destroy(b);
}

int main(void) {
    test_json_builder_null_guards();
    test_json_builder_basic();
    test_json_builder_escaping();
    test_json_builder_array_and_null_value();
    printf("All test_json_builder tests passed!\n");
    return 0;
}
