/**
 * @file test_rdbms_auto_mount.c
 * @brief Unit tests for RDBMS auto-detection and runtime mounting.
 */

#include <assert.h>
#include <string.h>
#include <stdio.h>

#include <cwist/sys/app/app.h>

static void test_null_app(void)
{
    bool ok = cwist_app_auto_rdbms(NULL, 5432);
    assert(!ok);
    printf("PASS: null app returns false\n");
}

static void test_unbound_port(void)
{
    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    /* Use a port that is extremely unlikely to be bound */
    bool ok = cwist_app_auto_rdbms(app, 54320);
    assert(!ok);
    assert(app->rdbms == NULL);

    cwist_app_destroy(app);
    printf("PASS: unbound port returns false and leaves rdbms NULL\n");
}

static void test_probe_unbound_port(void)
{
    cwist_rdbms_provider_t p = cwist_rdbms_probe_port(54320);
    assert(p == CWIST_RDBMS_NONE);
    printf("PASS: probe on unbound port returns NONE\n");
}

static void test_double_mount(void)
{
    cwist_app *app = cwist_app_create();
    assert(app != NULL);

    /* Manually mount once */
    bool ok = cwist_rdbms_mount_runtime(app, CWIST_RDBMS_POSTGRES, 5432);
    assert(ok);
    assert(app->rdbms != NULL);
    assert(app->rdbms->provider == CWIST_RDBMS_POSTGRES);
    assert(app->rdbms->port == 5432);
    assert(app->rdbms->ready == true);

    /* Second mount should succeed (idempotent) */
    ok = cwist_rdbms_mount_runtime(app, CWIST_RDBMS_MYSQL, 3306);
    assert(ok);
    /* Existing runtime should be left untouched */
    assert(app->rdbms->provider == CWIST_RDBMS_POSTGRES);
    assert(app->rdbms->port == 5432);

    cwist_app_destroy(app);
    printf("PASS: double mount is idempotent\n");
}

static void test_public_api_declaration(void)
{
    /* Ensure the public header declares the function and the struct field */
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    (void)app->rdbms; /* void* in public header */
    cwist_app_destroy(app);
    printf("PASS: public API field accessible\n");
}

int main(void)
{
    test_null_app();
    test_unbound_port();
    test_probe_unbound_port();
    test_double_mount();
    test_public_api_declaration();
    printf("All RDBMS auto-mount tests passed.\n");
    return 0;
}
