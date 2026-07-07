/**
 * @file cookie.h
 * @brief HTTP cookie parsing and construction helpers.
 */

#ifndef __CWIST_COOKIE_H__
#define __CWIST_COOKIE_H__

#include <cwist/net/http/http.h>
#include <cwist/query.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse a Cookie header value into a query-style map.
 *
 * Handles "name1=value1; name2=value2" format. Values are URL-decoded.
 *
 * @param map   Map to populate. Must be created by cwist_query_map_create().
 * @param header Raw Cookie header value (may be NULL).
 */
void cwist_cookie_parse(cwist_query_map *map, const char *header);

/**
 * @brief URL-encode a string for safe cookie values.
 * @return Heap-encoded string, or NULL on allocation failure. Caller frees.
 */
char *cwist_cookie_encode(const char *value);

/**
 * @brief URL-decode a cookie value in-place into a caller-owned buffer.
 * @param in  Encoded value.
 * @param out Output buffer.
 * @param out_len Size of output buffer.
 * @return Number of bytes written, or -1 on overflow.
 */
int cwist_cookie_decode(const char *in, char *out, size_t out_len);

/**
 * @brief Options for setting a response cookie.
 */
typedef struct cwist_cookie_options {
    const char *path;
    const char *domain;
    int max_age_seconds;   /**< < 0 means omit */
    bool http_only;
    bool secure;
    const char *same_site; /**< "Strict", "Lax", or "None" */
} cwist_cookie_options;

/**
 * @brief Add a Set-Cookie header to the response.
 *
 * @param res      Response to modify.
 * @param name     Cookie name.
 * @param value    Cookie value (will be encoded).
 * @param opts     Optional attributes. May be NULL for defaults.
 * @return 0 on success, -1 on failure.
 */
int cwist_cookie_set(cwist_http_response *res,
                     const char *name,
                     const char *value,
                     const cwist_cookie_options *opts);

/**
 * @brief Add a Set-Cookie header that deletes a cookie.
 */
int cwist_cookie_delete(cwist_http_response *res, const char *name);

/**
 * @brief Get a cookie value by name from a parsed map.
 * @return Pointer to value, or NULL if not found.
 */
const char *cwist_cookie_get(cwist_query_map *map, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_COOKIE_H__ */
