#ifndef __CWIST_MULTIPART_H__
#define __CWIST_MULTIPART_H__

#include <stddef.h>

/**
 * @file multipart.h
 * @brief RFC 7578 multipart/form-data parser.
 */

typedef struct cwist_multipart_field {
    char *name;           ///< Form field name.
    char *filename;       ///< File name (NULL for non-file fields).
    char *content_type;   ///< Content-Type of the part (NULL if absent).
    char *data;           ///< Raw part body (may contain binary data).
    size_t data_len;      ///< Length of @p data in bytes.
    struct cwist_multipart_field *next;
} cwist_multipart_field;

typedef struct cwist_multipart_result {
    cwist_multipart_field *fields; ///< Head of the field linked list.
} cwist_multipart_result;

/**
 * @brief Parse a multipart/form-data body.
 *
 * @param body       Raw body buffer (may contain binary data).
 * @param body_len   Length of @p body in bytes.
 * @param boundary   Boundary string (without leading hyphens).
 * @return Parsed result, or NULL on allocation failure or malformed input.
 */
cwist_multipart_result *cwist_multipart_parse(const char *body, size_t body_len, const char *boundary);

/**
 * @brief Free all memory associated with a multipart parse result.
 * @param result Result to destroy. NULL is ignored.
 */
void cwist_multipart_result_destroy(cwist_multipart_result *result);

/**
 * @brief Extract the boundary string from a Content-Type header value.
 *
 * @param content_type Content-Type header value, e.g.
 *        "multipart/form-data; boundary=----WebKitFormBoundary..."
 * @return Heap-allocated boundary string (caller must free), or NULL if not found.
 */
char *cwist_multipart_extract_boundary(const char *content_type);

#endif
