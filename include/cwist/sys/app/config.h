#ifndef __CWIST_CONFIG_H__
#define __CWIST_CONFIG_H__

/**
 * @file config.h
 * @brief Application configuration management (.env, environment variables).
 */

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
cwist_config *cwist_config_create(void);
void cwist_config_destroy(cwist_config *cfg);
/** @} */

/** @name Loading */
/** @{ */
void cwist_config_load_env(cwist_config *cfg, const char *prefix);
void cwist_config_load_file(cwist_config *cfg, const char *path);
/** @} */

/** @name Access */
/** @{ */
const char *cwist_config_get(cwist_config *cfg, const char *key);
int cwist_config_get_int(cwist_config *cfg, const char *key, int default_val);
bool cwist_config_get_bool(cwist_config *cfg, const char *key, bool default_val);
void cwist_config_set(cwist_config *cfg, const char *key, const char *value);
/** @} */

#endif
