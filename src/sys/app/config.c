/** @file config.c
 * @brief config.c interface.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/config.h>
#include <cwist/core/mem/alloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define CWIST_CONFIG_BUCKETS 64

static size_t cwist_config_hash(const char *key) {
    size_t hash = 5381;
    int c;
    while ((c = *key++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

cwist_config *cwist_config_create(void) {
    cwist_config *cfg = (cwist_config *)cwist_alloc(sizeof(cwist_config));
    if (!cfg) return NULL;
    cfg->bucket_count = CWIST_CONFIG_BUCKETS;
    cfg->buckets = (cwist_config_bucket **)cwist_alloc_array(cfg->bucket_count, sizeof(cwist_config_bucket *));
    if (!cfg->buckets) {
        cwist_free(cfg);
        return NULL;
    }
    return cfg;
}

void cwist_config_destroy(cwist_config *cfg) {
    if (!cfg) return;
    for (size_t i = 0; i < cfg->bucket_count; i++) {
        cwist_config_bucket *curr = cfg->buckets[i];
        while (curr) {
            cwist_config_bucket *next = curr->next;
            cwist_free(curr->key);
            cwist_free(curr->value);
            cwist_free(curr);
            curr = next;
        }
    }
    cwist_free(cfg->buckets);
    cwist_free(cfg);
}

void cwist_config_set(cwist_config *cfg, const char *key, const char *value) {
    if (!cfg || !key) return;
    size_t idx = cwist_config_hash(key) % cfg->bucket_count;
    cwist_config_bucket *curr = cfg->buckets[idx];
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            cwist_free(curr->value);
            curr->value = cwist_strdup(value ? value : "");
            return;
        }
        curr = curr->next;
    }
    cwist_config_bucket *bucket = (cwist_config_bucket *)cwist_alloc(sizeof(cwist_config_bucket));
    bucket->key = cwist_strdup(key);
    bucket->value = cwist_strdup(value ? value : "");
    bucket->next = cfg->buckets[idx];
    cfg->buckets[idx] = bucket;
}

const char *cwist_config_get(cwist_config *cfg, const char *key) {
    if (!cfg || !key) return NULL;
    size_t idx = cwist_config_hash(key) % cfg->bucket_count;
    cwist_config_bucket *curr = cfg->buckets[idx];
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            return curr->value;
        }
        curr = curr->next;
    }
    return NULL;
}

int cwist_config_get_int(cwist_config *cfg, const char *key, int default_val) {
    const char *val = cwist_config_get(cfg, key);
    if (!val) return default_val;
    char *endptr;
    long l = strtol(val, &endptr, 10);
    if (endptr == val || *endptr != '\0') return default_val;
    return (int)l;
}

bool cwist_config_get_bool(cwist_config *cfg, const char *key, bool default_val) {
    const char *val = cwist_config_get(cfg, key);
    if (!val) return default_val;
    if (strcasecmp(val, "true") == 0 || strcasecmp(val, "1") == 0 || strcasecmp(val, "yes") == 0)
        return true;
    if (strcasecmp(val, "false") == 0 || strcasecmp(val, "0") == 0 || strcasecmp(val, "no") == 0)
        return false;
    return default_val;
}

void cwist_config_load_env(cwist_config *cfg, const char *prefix) {
    if (!cfg) return;
    extern char **environ;
    size_t prefix_len = prefix ? strlen(prefix) : 0;
    for (char **env = environ; *env; env++) {
        char *eq = strchr(*env, '=');
        if (!eq) continue;
        size_t key_len = (size_t)(eq - *env);
        if (prefix_len > 0) {
            if (key_len < prefix_len || strncmp(*env, prefix, prefix_len) != 0)
                continue;
        }
        char *key = (char *)cwist_alloc(key_len + 1);
        memcpy(key, *env, key_len);
        key[key_len] = '\0';
        cwist_config_set(cfg, key, eq + 1);
        cwist_free(key);
    }
}

void cwist_config_load_file(cwist_config *cfg, const char *path) {
    if (!cfg || !path) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *value = eq + 1;
        // trim key
        char *ke = key + strlen(key) - 1;
        while (ke > key && isspace((unsigned char)*ke)) *ke-- = '\0';
        // trim value
        while (isspace((unsigned char)*value)) value++;
        char *ve = value + strlen(value) - 1;
        while (ve > value && isspace((unsigned char)*ve)) *ve-- = '\0';
        // unquote value
        if (value[0] == '"' && ve == value + strlen(value) - 1 && *ve == '"') {
            value++;
            *ve = '\0';
        }
        cwist_config_set(cfg, key, value);
    }
    fclose(f);
}
