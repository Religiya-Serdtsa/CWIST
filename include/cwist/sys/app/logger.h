/**
 * @file logger.h
 * @brief Application-level structured logging.
 */

#ifndef __CWIST_LOGGER_H__
#define __CWIST_LOGGER_H__

#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

typedef enum {
    CWIST_LOG_DEBUG,
    CWIST_LOG_INFO,
    CWIST_LOG_WARN,
    CWIST_LOG_ERROR,
} cwist_log_level_t;

typedef struct cwist_logger {
    char *name;
    cwist_log_level_t level;
    bool use_colors;
} cwist_logger;

/** @name Lifecycle */
/** @{ */

/**
 * @brief Create a logger.
 */
cwist_logger *cwist_logger_create(const char *name);

/**
 * @brief Destroy a logger.
 */
void cwist_logger_destroy(cwist_logger *log);

/** @} */

/** @name Configuration */
/** @{ */

/**
 * @brief Set the minimum log level.
 */
void cwist_logger_set_level(cwist_logger *log, cwist_log_level_t level);

/**
 * @brief Enable or disable colored output.
 */
void cwist_logger_set_colors(cwist_logger *log, bool enabled);

/** @} */

/** @name Logging */
/** @{ */

/**
 * @brief Write a log message.
 */
void cwist_logger_log(cwist_logger *log, cwist_log_level_t level, const char *fmt, ...);

/**
 * @brief Write a debug message.
 */
void cwist_logger_debug(cwist_logger *log, const char *fmt, ...);

/**
 * @brief Write an info message.
 */
void cwist_logger_info(cwist_logger *log, const char *fmt, ...);

/**
 * @brief Write a warning message.
 */
void cwist_logger_warn(cwist_logger *log, const char *fmt, ...);

/**
 * @brief Write an error message.
 */
void cwist_logger_error(cwist_logger *log, const char *fmt, ...);

/** @} */

#endif
