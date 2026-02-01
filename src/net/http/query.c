#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/query.h>
#include <cwist/core/siphash/siphash.h>
#include <cwist/core/mem/alloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <uriparser/Uri.h>

#define CWIST_QUERY_MAP_DEFAULT_SIZE 16

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

void cwist_query_map_destroy(cwist_query_map *map) {
    if (!map) return;
    cwist_query_map_clear(map);
    cwist_free(map->buckets);
    cwist_free(map);
}

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

void cwist_query_map_parse(cwist_query_map *map, const char *raw_query) {
    if (!map || !raw_query || strlen(raw_query) == 0) return;

    UriQueryListA *queryList = NULL;
    int itemCount = 0;
    
    // uriparser handles & and = and url decoding
    if (uriDissectQueryMallocA(&queryList, &itemCount, raw_query, raw_query + strlen(raw_query)) != URI_SUCCESS) {
        return;
    }

    UriQueryListA *curr = queryList;
    while (curr) {
        if (curr->key) {
            cwist_query_map_set(map, curr->key, curr->value ? curr->value : "");
        }
        curr = curr->next;
    }

    uriFreeQueryListA(queryList);
}
