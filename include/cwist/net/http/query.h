#ifndef __CWIST_QUERY_H__
#define __CWIST_QUERY_H__

#include <stdint.h>
#include <stddef.h>

typedef struct cwist_query_bucket {
    char *key;
    char *value;
    struct cwist_query_bucket *next;
} cwist_query_bucket;

typedef struct cwist_query_map {
    cwist_query_bucket **buckets;
    size_t size;
    uint8_t seed[16];
} cwist_query_map;

// Lifecycle
cwist_query_map *cwist_query_map_create(void);
void cwist_query_map_destroy(cwist_query_map *map);

// Parse raw query string (e.g., "a=1&b=2") into map
void cwist_query_map_parse(cwist_query_map *map, const char *raw_query);
void cwist_query_map_clear(cwist_query_map *map);

// Access
const char *cwist_query_map_get(cwist_query_map *map, const char *key);
void cwist_query_map_set(cwist_query_map *map, const char *key, const char *value);

#endif
