#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/query.h>
#include <cwist/core/siphash/siphash.h>
#include <cwist/core/mem/alloc.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/**
 * @file query.c
 * @brief Hash-map based storage and parsing helpers for URL query parameters.
 */

#define CWIST_QUERY_MAP_DEFAULT_SIZE 16

/**
 * @brief Allocate a query map and seed its SipHash key material.
 * @return Newly allocated map, or NULL when memory allocation fails.
 */
cwist_query_map *cwist_query_map_create(void) {
    cwist_query_map *map = (cwist_query_map *)cwist_alloc(sizeof(cwist_query_map));
    if (!map) return NULL;

    map->size = CWIST_QUERY_MAP_DEFAULT_SIZE;
    map->buckets = (cwist_query_bucket **)cwist_alloc_array(map->size, sizeof(cwist_query_bucket *));
    if (!map->buckets) {
        cwist_free(map);
        return NULL;
    }

    cwist_generate_hash_seed(map->seed);
    return map;
}

/**
 * @brief Release every bucket node and the map container itself.
 * @param map Query map to destroy.
 */
void cwist_query_map_destroy(cwist_query_map *map) {
    if (!map) return;
    cwist_query_map_clear(map);
    cwist_free(map->buckets);
    cwist_free(map);
}

/**
 * @brief Remove every key/value pair while keeping the bucket array allocated.
 * @param map Query map to clear in-place.
 */
void cwist_query_map_clear(cwist_query_map *map) {
    if (!map) return;
    for (size_t i = 0; i < map->size; i++) {
        cwist_query_bucket *curr = map->buckets[i];
        while (curr) {
            cwist_query_bucket *next = curr->next;
            cwist_free(curr->key);
            cwist_free(curr->value);
            cwist_free(curr);
            curr = next;
        }
        map->buckets[i] = NULL;
    }
}

/**
 * @brief Insert or replace a decoded query parameter in the map.
 * @param map Query map that owns the entry storage.
 * @param key Decoded query-string key.
 * @param value Decoded query-string value.
 */
void cwist_query_map_set(cwist_query_map *map, const char *key, const char *value) {
    if (!map || !key || !value) return;

    uint64_t hash = siphash24(key, strlen(key), map->seed);
    size_t index = hash % map->size;

    cwist_query_bucket *curr = map->buckets[index];
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            // Update existing
            cwist_free(curr->value);
            curr->value = cwist_strdup(value);
            return;
        }
        curr = curr->next;
    }

    // Insert new
    cwist_query_bucket *node = (cwist_query_bucket *)cwist_alloc(sizeof(cwist_query_bucket));
    if (!node) return;
    node->key = cwist_strdup(key);
    node->value = cwist_strdup(value);
    node->next = map->buckets[index];
    map->buckets[index] = node;
}

/**
 * @brief Look up a decoded query parameter by key.
 * @param map Query map to search.
 * @param key Decoded key to retrieve.
 * @return Stored value, or NULL when the key is absent.
 */
const char *cwist_query_map_get(cwist_query_map *map, const char *key) {
    if (!map || !key) return NULL;

    uint64_t hash = siphash24(key, strlen(key), map->seed);
    size_t index = hash % map->size;

    cwist_query_bucket *curr = map->buckets[index];
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            return curr->value;
        }
        curr = curr->next;
    }
    return NULL;
}

void cwist_query_map_delete(cwist_query_map *map, const char *key) {
    if (!map || !key) return;

    uint64_t hash = siphash24(key, strlen(key), map->seed);
    size_t index = hash % map->size;

    cwist_query_bucket *curr = map->buckets[index];
    cwist_query_bucket *prev = NULL;
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            if (prev) {
                prev->next = curr->next;
            } else {
                map->buckets[index] = curr->next;
            }
            cwist_free(curr->key);
            cwist_free(curr->value);
            cwist_free(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

void cwist_query_map_foreach(cwist_query_map *map, cwist_query_map_iter_func cb, void *ctx) {
    if (!map || !cb) return;
    for (size_t i = 0; i < map->size; i++) {
        cwist_query_bucket *curr = map->buckets[i];
        while (curr) {
            cb(curr->key, curr->value, ctx);
            curr = curr->next;
        }
    }
}

/**
 * @brief Decode a URL-encoded component with '+' → space semantics.
 *        '+' → 0x20, '%XY' → hex byte (strict), everything else passthrough.
 * @param src Raw (still encoded) key or value.
 * @return Newly allocated decoded string, or NULL on allocation failure.
 */
static char *url_decode(const char *src) {
    if (!src) return NULL;

    size_t len = strlen(src);
    char *out = (char *)cwist_alloc(len + 1);
    if (!out) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '%' && i + 2 < len
            && isxdigit((unsigned char)src[i + 1])
            && isxdigit((unsigned char)src[i + 2])) {
            unsigned int byte;
            sscanf(src + i + 1, "%2x", &byte);
            out[j++] = (char)byte;
            i += 2;
        } else if (src[i] == '+') {
            out[j++] = 0x20; /* SPACE (U+0020) */
        } else {
            out[j++] = src[i];
        }
    }
    out[j] = '\0';
    return out;
}

/**
 * @brief Parse a raw `a=1&b=2` query string into the existing map.
 * @param map Destination map that receives decoded keys and values.
 * @param raw_query Raw query substring without the leading question mark.
 */
void cwist_query_map_parse(cwist_query_map *map, const char *raw_query) {
    if (!map || !raw_query || strlen(raw_query) == 0) return;

    char *buffer = cwist_strdup(raw_query);
    if (!buffer) return;

    char *save_ptr = NULL;
    char *token = strtok_r(buffer, "&", &save_ptr);

    while (token) {
        char *eq = strchr(token, '=');
        if (!eq) {
            token = strtok_r(NULL, "&", &save_ptr);
            continue;
        }

        *eq = '\0';
        const char *key_raw = token;
        const char *value_raw = eq + 1;

        char *key_dec = url_decode(key_raw);
        char *value_dec = url_decode(value_raw);

        if (key_dec && cwist_query_map_get(map, key_dec) == NULL) {
            cwist_query_map_set(map, key_dec, value_dec ? value_dec : "");
        }

        cwist_free(key_dec);
        cwist_free(value_dec);
        token = strtok_r(NULL, "&", &save_ptr);
    }

    cwist_free(buffer);
}
