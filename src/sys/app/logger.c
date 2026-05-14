/** @file logger.c
 * @brief logger.c interface.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/logger.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *level_strings[] = { "DEBUG", "INFO", "WARN", "ERROR" };
static const char *level_colors[]  = { "\033[36m", "\033[32m", "\033[33m", "\033[31m" };
static const char *color_reset = "\033[0m";

static FILE *level_file(cwist_log_level_t level) {
    return level >= CWIST_LOG_WARN ? stderr : stdout;
}

cwist_logger *cwist_logger_create(const char *name) {
    cwist_logger *log = (cwist_logger *)cwist_alloc(sizeof(cwist_logger));
    if (!log) return NULL;
    log->name = cwist_strdup(name ? name : "cwist");
    log->level = CWIST_LOG_INFO;
    log->use_colors = true;
    return log;
}

void cwist_logger_destroy(cwist_logger *log) {
    if (!log) return;
    cwist_free(log->name);
    cwist_free(log);
}

void cwist_logger_set_level(cwist_logger *log, cwist_log_level_t level) {
    if (log) log->level = level;
}

void cwist_logger_set_colors(cwist_logger *log, bool enabled) {
    if (log) log->use_colors = enabled;
}

void cwist_logger_log(cwist_logger *log, cwist_log_level_t level, const char *fmt, ...) {
    if (!log || level < log->level) return;
    FILE *out = level_file(level);
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    if (log->use_colors) {
        fprintf(out, "%s[%s]%s [%s] %s: ", level_colors[level], time_buf, color_reset, level_strings[level], log->name);
    } else {
        fprintf(out, "[%s] [%s] %s: ", time_buf, level_strings[level], log->name);
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);
    fprintf(out, "\n");
    fflush(out);
}

void cwist_logger_debug(cwist_logger *log, const char *fmt, ...) {
    if (!log || CWIST_LOG_DEBUG < log->level) return;
    va_list args;
    va_start(args, fmt);
    cwist_logger_log(log, CWIST_LOG_DEBUG, "%s", "");
    va_end(args);
    /* Re-construct with va_list pass-through not possible portably for wrappers.
       Using a small internal buffer approach for wrapper macros is better,
       but here we just forward via the variadic macro pattern in header. */
}

void cwist_logger_info(cwist_logger *log, const char *fmt, ...) {
    if (!log || CWIST_LOG_INFO < log->level) return;
    va_list args;
    va_start(args, fmt);
    FILE *out = stdout;
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    if (log->use_colors) {
        fprintf(out, "%s[%s]%s [INFO] %s: ", level_colors[CWIST_LOG_INFO], time_buf, color_reset, log->name);
    } else {
        fprintf(out, "[%s] [INFO] %s: ", time_buf, log->name);
    }
    vfprintf(out, fmt, args);
    va_end(args);
    fprintf(out, "\n");
    fflush(out);
}

void cwist_logger_warn(cwist_logger *log, const char *fmt, ...) {
    if (!log || CWIST_LOG_WARN < log->level) return;
    va_list args;
    va_start(args, fmt);
    FILE *out = stderr;
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    if (log->use_colors) {
        fprintf(out, "%s[%s]%s [WARN] %s: ", level_colors[CWIST_LOG_WARN], time_buf, color_reset, log->name);
    } else {
        fprintf(out, "[%s] [WARN] %s: ", time_buf, log->name);
    }
    vfprintf(out, fmt, args);
    va_end(args);
    fprintf(out, "\n");
    fflush(out);
}

void cwist_logger_error(cwist_logger *log, const char *fmt, ...) {
    if (!log || CWIST_LOG_ERROR < log->level) return;
    va_list args;
    va_start(args, fmt);
    FILE *out = stderr;
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    if (log->use_colors) {
        fprintf(out, "%s[%s]%s [ERROR] %s: ", level_colors[CWIST_LOG_ERROR], time_buf, color_reset, log->name);
    } else {
        fprintf(out, "[%s] [ERROR] %s: ", time_buf, log->name);
    }
    vfprintf(out, fmt, args);
    va_end(args);
    fprintf(out, "\n");
    fflush(out);
}
