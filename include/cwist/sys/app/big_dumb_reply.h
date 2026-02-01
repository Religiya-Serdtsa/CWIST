/**
 * @file big_dumb_reply.h
 * @brief Auto-Caching Layer for repetitive requests.
 */

#ifndef __CWIST_BIG_DUMB_REPLY_H__
#define __CWIST_BIG_DUMB_REPLY_H__

#include <cwist/core/sstring/sstring.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/**
 * @brief Big Dumb Reply Entry.
 * Stores a completely serialized HTTP response blob.
 */
typedef struct bdr_entry_t {
    uint64_t request_hash; ///< Key: SipHash(Method + Path)
    
    uint64_t response_hash;///< Hash of the response content (for stability check)
    bool is_stable;        ///< True if response proved stable across requests
    
    void *response_blob;   ///< Complete HTTP response (headers + body)
    size_t len;            ///< Length of blob
    
    uint64_t hits;         ///< Hit count
    time_t created_at;     ///< Creation timestamp
    
    struct bdr_entry_t *next;
} bdr_entry_t;

/**
 * @brief Big Dumb Reply Context.
 * Manages cache buckets and learning parameters.
 */
typedef struct cwist_bdr_t {
    bdr_entry_t **buckets;     ///< Hash buckets
    size_t bucket_count;       ///< Number of buckets
    
    /// Learning configuration parameters.
    int hit_threshold;         ///< (Unused) Hits before caching
    int latency_threshold_ms;  ///< Latency threshold to trigger caching

    size_t current_bytes;      ///< Total bytes stored in-memory
    size_t max_bytes;          ///< Soft limit for cached response bytes
    time_t max_entry_age_sec;  ///< TTL for cached replies (0 = no TTL)
    uint64_t revalidate_hits;  ///< Force refresh after this many hits
    size_t gc_cursor;          ///< Round-robin sweep cursor
    
    /// Fallback disk database mode.
    struct sqlite3 *disk_db;   ///< Disk DB handle for low-RAM mode
    bool is_disk_mode;         ///< True if fallback is active
} cwist_bdr_t;

/**
 * @brief Creates a BDR context.
 */
cwist_bdr_t *cwist_bdr_create(void);

/**
 * @brief Destroys a BDR context and frees all cached blobs.
 */
void cwist_bdr_destroy(cwist_bdr_t *bdr);

/**
 * @brief Try to find a cached response.
 * @param bdr Context.
 * @param method HTTP Method (only GET supported).
 * @param path Request path.
 * @param out_len [out] Length of the found blob.
 * @return Pointer to the blob if found, NULL otherwise.
 */
const void *cwist_bdr_get(cwist_bdr_t *bdr, const char *method, const char *path, size_t *out_len);

/**
 * @brief Store a response in the cache.
 * @param bdr Context.
 * @param method HTTP Method.
 * @param path Request path.
 * @param data Serialized response data.
 * @param len Length of data.
 */
void cwist_bdr_put(cwist_bdr_t *bdr, const char *method, const char *path, const void *data, size_t len);

/**
 * @brief Adjusts guard-rail policies for the in-memory cache.
 * @param bdr Context.
 * @param max_bytes Maximum bytes to keep in RAM (0 keeps default).
 * @param max_entry_age_sec Time-to-live for cached entries (<=0 keeps default).
 * @param revalidate_hits Force relearning after this many hits (0 keeps default).
 */
void cwist_bdr_set_limits(cwist_bdr_t *bdr, size_t max_bytes, time_t max_entry_age_sec, uint64_t revalidate_hits);

#endif
