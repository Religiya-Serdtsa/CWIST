/**
 * @file healthz.c
 * @brief Health probe registry and JSON endpoint generation.
 */

#include <cwist/sys/health/healthz.h>
#include <cwist/core/utils/json_builder.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/mem/alloc.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Internal probe registry
 * ---------------------------------------------------------------------- */

#define CWIST_HEALTHZ_MAX_PROBES 16

typedef struct cwist_healthz_entry {
    const char *name;
    cwist_health_probe_fn fn;
    void *ctx;
    bool active;
} cwist_healthz_entry_t;

static cwist_healthz_entry_t g_entries[CWIST_HEALTHZ_MAX_PROBES];
static int g_entry_count = 0;

/* -------------------------------------------------------------------------
 * Registration
 * ---------------------------------------------------------------------- */

bool cwist_healthz_register(const char *name, cwist_health_probe_fn fn, void *ctx) {
    if (!name || !fn) return false;
    if (g_entry_count >= CWIST_HEALTHZ_MAX_PROBES) return false;

    for (int i = 0; i < g_entry_count; ++i) {
        if (g_entries[i].active && strcmp(g_entries[i].name, name) == 0) {
            g_entries[i].fn = fn;
            g_entries[i].ctx = ctx;
            return true;
        }
    }

    g_entries[g_entry_count].name   = name;
    g_entries[g_entry_count].fn     = fn;
    g_entries[g_entry_count].ctx    = ctx;
    g_entries[g_entry_count].active = true;
    g_entry_count++;
    return true;
}

void cwist_healthz_unregister(const char *name) {
    if (!name) return;
    for (int i = 0; i < g_entry_count; ++i) {
        if (g_entries[i].active && strcmp(g_entries[i].name, name) == 0) {
            g_entries[i].active = false;
        }
    }
}

/* -------------------------------------------------------------------------
 * Evaluation
 * ---------------------------------------------------------------------- */

static const char *status_str(cwist_health_status_t s) {
    switch (s) {
        case CWIST_HEALTH_OK:       return "ok";
        case CWIST_HEALTH_DEGRADED: return "degraded";
        case CWIST_HEALTH_FAIL:     return "fail";
        default:                    return "unknown";
    }
}

void cwist_healthz_run(cwist_health_probe_t *out_probes,
                        size_t max_probes,
                        size_t *out_count,
                        cwist_health_status_t *out_overall) {
    size_t count = 0;
    cwist_health_status_t overall = CWIST_HEALTH_OK;

    for (int i = 0; i < g_entry_count && count < max_probes; ++i) {
        if (!g_entries[i].active) continue;
        cwist_health_probe_t r = g_entries[i].fn(g_entries[i].ctx);
        out_probes[count++] = r;
        if (r.status == CWIST_HEALTH_FAIL) overall = CWIST_HEALTH_FAIL;
        else if (r.status == CWIST_HEALTH_DEGRADED && overall == CWIST_HEALTH_OK)
            overall = CWIST_HEALTH_DEGRADED;
    }

    if (out_count) *out_count = count;
    if (out_overall) *out_overall = overall;
}

/* -------------------------------------------------------------------------
 * HTTP response helper
 * ---------------------------------------------------------------------- */

void cwist_app_healthz(cwist_http_response *res) {
    if (!res) return;

    cwist_health_probe_t probes[CWIST_HEALTHZ_MAX_PROBES];
    size_t count = 0;
    cwist_health_status_t overall = CWIST_HEALTH_OK;
    cwist_healthz_run(probes, CWIST_HEALTHZ_MAX_PROBES, &count, &overall);

    cwist_json_builder *jb = cwist_json_builder_create();
    cwist_json_begin_object(jb);
    cwist_json_add_string(jb, "status", status_str(overall));
    cwist_json_begin_array(jb, "probes");
    for (size_t i = 0; i < count; ++i) {
        cwist_json_begin_object(jb);
        cwist_json_add_string(jb, "name", probes[i].name);
        cwist_json_add_string(jb, "status", status_str(probes[i].status));
        if (probes[i].message) {
            cwist_json_add_string(jb, "message", probes[i].message);
        }
        cwist_json_end_object(jb);
    }
    cwist_json_end_array(jb);
    cwist_json_end_object(jb);

    const char *json = cwist_json_get_raw(jb);
    cwist_sstring_assign(res->body, (char *)json);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");

    switch (overall) {
        case CWIST_HEALTH_OK:       res->status_code = CWIST_HTTP_OK; break;
        case CWIST_HEALTH_DEGRADED: res->status_code = CWIST_HTTP_SERVICE_UNAVAILABLE; break;
        case CWIST_HEALTH_FAIL:     res->status_code = CWIST_HTTP_SERVICE_UNAVAILABLE; break;
    }
    cwist_json_builder_destroy(jb);
}
