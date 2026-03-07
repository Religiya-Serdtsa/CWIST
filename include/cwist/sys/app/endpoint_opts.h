/**
 * @file endpoint_opts.h
 * @brief Endpoint-level behavior flags for CWIST apps.
 */

#ifndef __CWIST_ENDPOINT_OPTS_H__
#define __CWIST_ENDPOINT_OPTS_H__

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Bitmask type controlling endpoint behavior.
 *
 * Flags can be OR-ed together to hint caching or streaming
 * strategies for a given route.
 */
typedef uint32_t cwist_endpoint_opt_t;

/** Immediate execution with no special tuning (default). */
#define CWIST_DYNAMIC         (1u << 0)
/** Cache the response in RAM and reply instantly after the first hit. */
#define CWIST_ENDPOINT_FIXED  (1u << 1)
/** Serve large files using OS-specific zero-copy fast paths. */
#define CWIST_ENDPOINT_FILE   (1u << 2)

/** @brief Default option for new endpoints. */
#define CWIST_ENDPOINT_DEFAULT (CWIST_DYNAMIC)

static inline bool cwist_endpoint_has(cwist_endpoint_opt_t opts, cwist_endpoint_opt_t flag) {
    return (opts & flag) != 0;
}

#endif /* __CWIST_ENDPOINT_OPTS_H__ */
