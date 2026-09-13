/**
 * @file test_flash.c
 * @brief Unit tests for flash messages, zero-guards, and json serialization.
 */

#include <cwist/sys/session/flash.h>
#include <cwist/net/http/http.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/mem/alloc.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

static void test_flash_null_and_zero_guards(void) {
    printf("test_flash_null_and_zero_guards...\n");

    // NULL request or NULL keys
    cwist_flash_set(NULL, "key", "val");
    assert(cwist_flash_peek(NULL, "key") == NULL);
    assert(cwist_flash_get(NULL, "key") == NULL);
    assert(cwist_flash_pop_all_json(NULL) == NULL);

    cwist_http_request req;
    memset(&req, 0, sizeof(req));

    // Empty / NULL flash
    assert(cwist_flash_peek(&req, "key") == NULL);
    assert(cwist_flash_get(&req, "key") == NULL);
    assert(cwist_flash_pop_all_json(&req) == NULL);

    // Dummy map with size 0: should not divide by zero
    cwist_query_map map;
    memset(&map, 0, sizeof(map));
    map.size = 0;
    map.buckets = NULL;
    req.flash = &map;

    assert(cwist_flash_get(&req, "key") == NULL);
    assert(cwist_flash_pop_all_json(&req) == NULL);
    req.flash = NULL;
}

static void test_flash_basic_and_json(void) {
    printf("test_flash_basic_and_json...\n");

    cwist_http_request req;
    memset(&req, 0, sizeof(req));

    cwist_flash_set(&req, "info", "File uploaded \"successfully\"\\test");
    cwist_flash_set(&req, "status", "active");

    // Peek does not remove
    assert(cwist_flash_peek(&req, "info") != NULL);
    assert(cwist_flash_peek(&req, "info") != NULL);

    // Pop all as JSON
    char *json = cwist_flash_pop_all_json(&req);
    assert(json != NULL);

    cJSON *parsed = cJSON_Parse(json);
    assert(parsed != NULL);
    assert(cJSON_GetObjectItem(parsed, "status") != NULL);
    assert(strcmp(cJSON_GetObjectItem(parsed, "status")->valuestring, "active") == 0);
    assert(cJSON_GetObjectItem(parsed, "info") != NULL);
    cJSON_Delete(parsed);
    cwist_free(json);

    // Flash entries should now be cleared
    assert(cwist_flash_peek(&req, "info") == NULL);
    assert(cwist_flash_pop_all_json(&req) == NULL);

    // Test cwist_flash_get pop
    cwist_flash_set(&req, "msg", "hello");
    const char *val = cwist_flash_get(&req, "msg");
    assert(val != NULL);
    assert(strcmp(val, "hello") == 0);
    cwist_free((void *)val);
    assert(cwist_flash_get(&req, "msg") == NULL);

    cwist_query_map_destroy(req.flash);
}

int main(void) {
    test_flash_null_and_zero_guards();
    test_flash_basic_and_json();
    printf("All test_flash tests passed!\n");
    return 0;
}
