/**
 * @file test_error.c
 * @brief Unit tests for make_error zero-initialization and error handling.
 */

#include <cwist/sys/err/cwist_err.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

static void test_make_error_zero_init(void) {
    printf("test_make_error_zero_init...\n");
    cwist_error_t err = make_error(CWIST_ERR_INT32);
    assert(err.errtype == CWIST_ERR_INT32);
    assert(err.error.err_i32 == 0);
    assert(cwist_error_is_ok(&err));

    cwist_error_t err_json = make_error(CWIST_ERR_JSON);
    assert(err_json.errtype == CWIST_ERR_JSON);
    assert(err_json.error.err_json == NULL);

    // Should not crash because err_json is safely NULL
    cwist_error_dispose(&err_json);
    assert(err_json.error.err_json == NULL);
}

static void test_error_dispose_cjson(void) {
    printf("test_error_dispose_cjson...\n");
    cwist_error_t err = make_error(CWIST_ERR_JSON);
    err.error.err_json = cJSON_CreateObject();
    assert(err.error.err_json != NULL);

    cwist_error_dispose(&err);
    assert(err.error.err_json == NULL);

    // NULL pointer should be ignored
    cwist_error_dispose(NULL);
}

int main(void) {
    test_make_error_zero_init();
    test_error_dispose_cjson();
    printf("All test_error tests passed!\n");
    return 0;
}
