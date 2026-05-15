/**
 * @file test_metrics.c
 * @brief Unit tests for metrics and healthz subsystems.
 */

#include <cwist/sys/metrics/metrics.h>
#include <cwist/sys/health/healthz.h>
#include <cwist/net/http/http.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

void test_metrics_counter(void) {
    printf("test_metrics_counter...\n");
    cwist_metrics_registry_t *reg = cwist_metrics_registry();
    cwist_metrics_reset(reg);

    cwist_metric_inc(reg, CWIST_METRIC_REQUESTS_TOTAL);
    cwist_metric_inc(reg, CWIST_METRIC_REQUESTS_TOTAL);
    cwist_metric_add(reg, CWIST_METRIC_REQUESTS_TOTAL, 3);

    uintmax_t v = cwist_metric_load(reg, CWIST_METRIC_REQUESTS_TOTAL);
    assert(v == 5);
    printf("  OK\n");
}

void test_metrics_gauge(void) {
    printf("test_metrics_gauge...\n");
    cwist_metrics_registry_t *reg = cwist_metrics_registry();
    cwist_metrics_reset(reg);

    cwist_metric_set(reg, CWIST_METRIC_FLOW_WINDOW_HEALTH, 87.5);
    uintmax_t v = cwist_metric_load(reg, CWIST_METRIC_FLOW_WINDOW_HEALTH);
    assert(v == 87500); /* scaled by 1000 */
    printf("  OK\n");
}

void test_metrics_prometheus_render(void) {
    printf("test_metrics_prometheus_render...\n");
    cwist_metrics_registry_t *reg = cwist_metrics_registry();
    cwist_metrics_reset(reg);

    cwist_metric_inc(reg, CWIST_METRIC_REQUESTS_TOTAL);
    cwist_metric_set(reg, CWIST_METRIC_FLOW_WINDOW_HEALTH, 100.0);

    char *text = cwist_metrics_render_prometheus(reg);
    assert(text != NULL);
    assert(strstr(text, "cwist_requests_total") != NULL);
    assert(strstr(text, "cwist_flow_window_health") != NULL);
    assert(strstr(text, "# HELP") != NULL);
    assert(strstr(text, "# TYPE") != NULL);
    free(text);
    printf("  OK\n");
}

void test_metrics_http_response(void) {
    printf("test_metrics_http_response...\n");
    cwist_metrics_registry_t *reg = cwist_metrics_registry();
    cwist_metrics_reset(reg);

    cwist_http_response *res = cwist_http_response_create();
    cwist_metrics_serve_http(res);
    assert(res->status_code == CWIST_HTTP_OK);
    assert(strstr(res->body->data, "cwist_") != NULL);
    cwist_http_response_destroy(res);
    printf("  OK\n");
}

static cwist_health_probe_t dummy_probe_ok(void *ctx) {
    (void)ctx;
    return (cwist_health_probe_t){ "dummy", CWIST_HEALTH_OK, "all good" };
}

static cwist_health_probe_t dummy_probe_fail(void *ctx) {
    (void)ctx;
    return (cwist_health_probe_t){ "dummy", CWIST_HEALTH_FAIL, "down" };
}

static cwist_health_probe_t dummy_probe_degraded(void *ctx) {
    (void)ctx;
    return (cwist_health_probe_t){ "dummy", CWIST_HEALTH_DEGRADED, "slow" };
}

void test_healthz_ok(void) {
    printf("test_healthz_ok...\n");
    cwist_healthz_register("dummy_ok", dummy_probe_ok, NULL);

    cwist_http_response *res = cwist_http_response_create();
    cwist_app_healthz(res);
    assert(res->status_code == CWIST_HTTP_OK);
    assert(strstr(res->body->data, "ok") != NULL);
    cwist_http_response_destroy(res);

    cwist_healthz_unregister("dummy_ok");
    printf("  OK\n");
}

void test_healthz_fail(void) {
    printf("test_healthz_fail...\n");
    cwist_healthz_register("dummy_fail", dummy_probe_fail, NULL);

    cwist_http_response *res = cwist_http_response_create();
    cwist_app_healthz(res);
    assert(res->status_code == CWIST_HTTP_SERVICE_UNAVAILABLE);
    assert(strstr(res->body->data, "fail") != NULL);
    cwist_http_response_destroy(res);

    cwist_healthz_unregister("dummy_fail");
    printf("  OK\n");
}

void test_healthz_degraded(void) {
    printf("test_healthz_degraded...\n");
    cwist_healthz_register("dummy_degraded", dummy_probe_degraded, NULL);

    cwist_http_response *res = cwist_http_response_create();
    cwist_app_healthz(res);
    assert(res->status_code == CWIST_HTTP_SERVICE_UNAVAILABLE);
    assert(strstr(res->body->data, "degraded") != NULL);
    cwist_http_response_destroy(res);

    cwist_healthz_unregister("dummy_degraded");
    printf("  OK\n");
}

#include <cwist/sys/app/middleware.h>

static void dummy_next_metrics(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_OK;
}

void test_metrics_middleware_wiring(void) {
    printf("test_metrics_middleware_wiring...\n");
    cwist_metrics_registry_t *reg = cwist_metrics_registry();
    cwist_metrics_reset(reg);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();

    cwist_middleware_func mw = cwist_mw_metrics();
    mw(req, res, dummy_next_metrics);

    uintmax_t total = cwist_metric_load(reg, CWIST_METRIC_REQUESTS_TOTAL);
    assert(total == 1);

    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    printf("  OK\n");
}

int main(void) {
    test_metrics_counter();
    test_metrics_gauge();
    test_metrics_prometheus_render();
    test_metrics_http_response();
    test_healthz_ok();
    test_healthz_fail();
    test_healthz_degraded();
    test_metrics_middleware_wiring();
    printf("All metrics/healthz tests passed.\n");
    return 0;
}
