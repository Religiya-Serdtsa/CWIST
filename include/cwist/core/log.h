/**
 * @file log.h
 * @brief Lightweight macro-based internal logging for CWIST.
 *
 * Zero-cost when disabled: if the current global level is below the log call's
 * level, no function call or argument evaluation occurs.
 */

#ifndef __CWIST_CORE_LOG_H__
#define __CWIST_CORE_LOG_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CWIST_LOG_LEVEL_NONE = 0,
    CWIST_LOG_LEVEL_DEBUG = 1,
    CWIST_LOG_LEVEL_INFO = 2,
    CWIST_LOG_LEVEL_WARN = 3,
    CWIST_LOG_LEVEL_ERROR = 4,
} cwist_macro_log_level_t;

/** @brief Global active log level. Set this to control output verbosity. */
extern cwist_macro_log_level_t g_cwist_log_level;

/**
 * @brief Initialize the logging system (e.g., set level from environment).
 */
void cwist_log_init(void);

/**
 * @brief Write a log message to stderr.
 * @param level Log level of this message.
 * @param fmt   printf-style format string.
 */
void cwist_log_write(cwist_macro_log_level_t level, const char *fmt, ...);

/**
 * @brief Main logging macro.
 *
 * Expands to a level check followed by a function call only when the global
 * level is at or above the requested level. Arguments are NOT evaluated when
 * the level is below threshold.
 */
#define CWIST_LOG(level, fmt, ...) \
    do { \
        if ((level) >= g_cwist_log_level && g_cwist_log_level > CWIST_LOG_LEVEL_NONE) { \
            cwist_log_write((level), fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define CWIST_LOG_INFO(fmt, ...)  CWIST_LOG(CWIST_LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)
#define CWIST_LOG_WARN(fmt, ...)  CWIST_LOG(CWIST_LOG_LEVEL_WARN,  fmt, ##__VA_ARGS__)
#define CWIST_LOG_ERROR(fmt, ...) CWIST_LOG(CWIST_LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define CWIST_LOG_DEBUG(fmt, ...) CWIST_LOG(CWIST_LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
