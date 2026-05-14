/**
 * @file config.h
 * @brief Application configuration management (.env, environment variables).
 */

#ifndef __CWIST_CONFIG_H__
#define __CWIST_CONFIG_H__

#include <stddef.h>
#include <stdbool.h>

typedef struct cwist_config_bucket {
    char *key;
    char *value;
    struct cwist_config_bucket *next;
} cwist_config_bucket;

typedef struct cwist_config {
    cwist_config_bucket **buckets;
    size_t bucket_count;
} cwist_config;

/** @name Lifecycle */
/** @{ */

/**
 * @brief Create a new configuration object.
 */
cwist_config *cwist_config_create(void);

/**
 * @brief Destroy a configuration object.
 */
void cwist_config_destroy(cwist_config *cfg);

/** @} */

/** @name Loading */
/** @{ */

/**
 * @brief Load environment variables with a prefix.
 */
void cwist_config_load_env(cwist_config *cfg, const char *prefix);

/**
 * @brief Load a configuration file.
 */
void cwist_config_load_file(cwist_config *cfg, const char *path);

/** @} */

/** @name Access */
/** @{ */

/**
 * @brief Get a string value by key.
 */
const char *cwist_config_get(cwist_config *cfg, const char *key);

/**
 * @brief Get an integer value by key.
 */
int cwist_config_get_int(cwist_config *cfg, const char *key, int default_val);

/**
 * @brief Get a boolean value by key.
 */
bool cwist_config_get_bool(cwist_config *cfg, const char *key, bool default_val);

/**
 * @brief Set a string value.
 */
void cwist_config_set(cwist_config *cfg, const char *key, const char *value);

/** @} */

#endif
