/**
 * @file test_log.c
 * @brief Test macro-based logging system.
 */

#include <cwist/core/log.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>

static const char g_log_template[] = "/tmp/cwist_test_log.XXXXXX";

static char *capture_logs(void (*cb)(void)) {
    char log_file[sizeof(g_log_template)];
    memcpy(log_file, g_log_template, sizeof(g_log_template));
    int fd = mkstemp(log_file);
    assert(fd >= 0);

    int saved_stderr = dup(STDERR_FILENO);
    assert(saved_stderr >= 0);

    assert(dup2(fd, STDERR_FILENO) >= 0);
    close(fd);

    cb();

    fflush(stderr);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);

    FILE *f = fopen(log_file, "r");
    assert(f != NULL);
    static char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    unlink(log_file);
    return buf;
}

static void log_at_all_levels(void) {
    CWIST_LOG_DEBUG("debug_message");
    CWIST_LOG_INFO("info_message");
    CWIST_LOG_WARN("warn_message");
}

int main(void) {
    printf("Testing macro logging...\n");

    /* NONE: no output */
    g_cwist_log_level = CWIST_LOG_LEVEL_NONE;
    char *out = capture_logs(log_at_all_levels);
    assert(strstr(out, "debug_message") == NULL);
    assert(strstr(out, "info_message") == NULL);
    assert(strstr(out, "warn_message") == NULL);

    /* DEBUG: all three */
    g_cwist_log_level = CWIST_LOG_LEVEL_DEBUG;
    out = capture_logs(log_at_all_levels);
    assert(strstr(out, "debug_message") != NULL);
    assert(strstr(out, "info_message") != NULL);
    assert(strstr(out, "warn_message") != NULL);

    /* INFO: info and warn */
    g_cwist_log_level = CWIST_LOG_LEVEL_INFO;
    out = capture_logs(log_at_all_levels);
    assert(strstr(out, "debug_message") == NULL);
    assert(strstr(out, "info_message") != NULL);
    assert(strstr(out, "warn_message") != NULL);

    /* WARN: only warn */
    g_cwist_log_level = CWIST_LOG_LEVEL_WARN;
    out = capture_logs(log_at_all_levels);
    assert(strstr(out, "debug_message") == NULL);
    assert(strstr(out, "info_message") == NULL);
    assert(strstr(out, "warn_message") != NULL);

    /* Verify format: [LEVEL] [Timestamp] ... */
    assert(strstr(out, "[WARN]") != NULL);
    assert(strstr(out, "[") != NULL); /* timestamp bracket */

    /* Reset to default */
    g_cwist_log_level = CWIST_LOG_LEVEL_NONE;

    printf("Passed macro logging test.\n");
    return 0;
}
