#ifndef __CWIST_MACROS_H__
#define __CWIST_MACROS_H__

/**
 * @file macros.h
 * @brief Convenience size-conversion and utility macros shared across CWIST.
 */

/** @brief Convert decimal kilobytes to bytes. */
#define CWIST_KB(x)  ((size_t)(x) * 1000)
/** @brief Convert binary kibibytes to bytes. */
#define CWIST_KIB(x) ((size_t)(x) * 1024)
/** @brief Convert decimal megabytes to bytes. */
#define CWIST_MB(x)  ((size_t)(x) * 1000 * 1000)
/** @brief Convert binary mebibytes to bytes. */
#define CWIST_MIB(x) ((size_t)(x) * 1024 * 1024)
/** @brief Convert decimal gigabytes to bytes. */
#define CWIST_GB(x)  ((size_t)(x) * 1000 * 1000 * 1000)
/** @brief Convert binary gibibytes to bytes. */
#define CWIST_GIB(x) ((size_t)(x) * 1024 * 1024 * 1024)

/** @brief Mark an intentionally unused variable to silence compiler warnings. */
#define CWIST_UNUSED(x) (void)(x)

#endif
