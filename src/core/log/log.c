/**
 * @file log.c
 * @brief Macro-based logging implementation.
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/core/log.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

cwist_macro_log_level_t g_cwist_log_level = CWIST_LOG_LEVEL_NONE;

void cwist_log_write(cwist_macro_log_level_t level, const char *fmt, ...) {
    const char *level_str = "???";
    switch (level) {
        case CWIST_LOG_LEVEL_INFO:  level_str = "INFO";  break;
        case CWIST_LOG_LEVEL_WARN:  level_str = "WARN";  break;
        case CWIST_LOG_LEVEL_ERROR: level_str = "ERROR"; break;
        case CWIST_LOG_LEVEL_DEBUG: level_str = "DEBUG"; break;
        default: break;
    }

    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    fprintf(stderr, "[%s] [%s] ", level_str, time_buf);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
}
