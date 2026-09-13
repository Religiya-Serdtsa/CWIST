/**
 * @file test_healthz.c
 * @brief Unit tests for health probe registry, slot reuse, and NULL safety.
 */

#include <cwist/sys/health/healthz.h>
#include <cwist/net/http/http.h>
#include <cwist/core/sstring/sstring.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* Stub for cwist_http_header_add to avoid linking full net/http subsystem */
cwist_error_t cwist_http_header_add(cwist_http_header_node **head, const char *key, const char *value) {
    (void)head; (void)key; (void)value;
    cwist_error_t err;
    memset(&err, 0, sizeof(err));
    err.errtype = CWIST_ERR_INT16;
    return err;
}

static cwist_health_probe_t probe_ok(void *ctx) {
    (void)ctx;
    return (cwist_health_probe_t){
        .name = "probe_ok",
        .status = CWIST_HEALTH_OK,
        .message = "All good"
    };
}

static cwist_health_probe_t probe_fail(void *ctx) {
    (void)ctx;
    return (cwist_health_probe_t){
        .name = "probe_fail",
        .status = CWIST_HEALTH_FAIL,
        .message = "Disk full"
    };
}

static void test_healthz_basic_and_slot_reuse(void) {
    printf("test_healthz_basic_and_slot_reuse...\n");

    // Register 16 probes to fill all slots
    char names[16][16];
    for (int i = 0; i < 16; ++i) {
        snprintf(names[i], sizeof(names[i]), "p_%d", i);
        assert(cwist_healthz_register(names[i], probe_ok, NULL));
    }

    // 17th probe should fail because table is full
    assert(!cwist_healthz_register("p_extra", probe_ok, NULL));

    // Unregister one probe
    cwist_healthz_unregister("p_5");

    // Now registering a new probe should succeed by reusing the inactive slot
    assert(cwist_healthz_register("p_reused", probe_ok, NULL));

    // Clean up probes for subsequent tests
    for (int i = 0; i < 16; ++i) {
        cwist_healthz_unregister(names[i]);
    }
    cwist_healthz_unregister("p_reused");
}

static void test_healthz_null_buffer_and_status(void) {
    printf("test_healthz_null_buffer_and_status...\n");

    assert(cwist_healthz_register("db", probe_ok, NULL));
    assert(cwist_healthz_register("storage", probe_fail, NULL));

    // Run with NULL out_probes: should not crash and should return overall status & count
    size_t count = 0;
    cwist_health_status_t overall = CWIST_HEALTH_OK;
    cwist_healthz_run(NULL, 0, &count, &overall);
    assert(count == 2);
    assert(overall == CWIST_HEALTH_FAIL);

    // Run with valid out_probes buffer
    cwist_health_probe_t probes[4];
    count = 0;
    overall = CWIST_HEALTH_OK;
    cwist_healthz_run(probes, 4, &count, &overall);
    assert(count == 2);
    assert(overall == CWIST_HEALTH_FAIL);

    // Test cwist_app_healthz response handling
    cwist_http_response res;
    memset(&res, 0, sizeof(res));
    res.body = cwist_sstring_create();
    cwist_app_healthz(&res);
    assert(res.status_code == CWIST_HTTP_SERVICE_UNAVAILABLE);
    assert(res.body != NULL);
    assert(strstr(res.body->data, "\"status\":\"fail\"") != NULL);
    cwist_sstring_destroy(res.body);

    cwist_healthz_unregister("db");
    cwist_healthz_unregister("storage");
}

int main(void) {
    test_healthz_basic_and_slot_reuse();
    test_healthz_null_buffer_and_status();
    printf("All test_healthz tests passed!\n");
    return 0;
}
