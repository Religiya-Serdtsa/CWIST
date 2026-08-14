/** @file sstring.h
 * @brief sstring.h interface.
 */
#ifndef __CWIST_SSTRING_H__
#define __CWIST_SSTRING_H__

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <cwist/err/cwist_err.h>

typedef struct cwist_sstring {
  char   *data;  ///< please access this data if raw handling is necessary
  bool   is_fixed;
  bool   owns_storage;
  bool   borrows_buffer; ///< data is borrowed (static/arena); never freed, detached on mutation
  size_t size;
  size_t (*get_size)(struct cwist_sstring *str);
  int     (*compare )(struct cwist_sstring *left, const struct cwist_sstring *right); ///< should mimic strcmp, internally use strncmp
  cwist_error_t (*copy  )(struct cwist_sstring *str, const struct cwist_sstring *from);
  cwist_error_t (*append)(struct cwist_sstring *str, const struct cwist_sstring *from);
                                   /// @brief returns 1 on success, returns 0 on failure
                                   /// @note should be used in this form:
                                   /// @code
                                   /// cwist_sstring str1;
                                   /// cwist_sstring str2;
                                   /// cwist_sstring_init(&str);
                                   /// cwist_sstring_init(&str2);
                                   /// cwist_error_t err = str1.copy(&str1, &str2);
                                   /// cwist_error_t err = str2.append(&str2, &str1);
                                   /// ...
                                   /// @endcode
} cwist_sstring;

cwist_sstring *cwist_sstring_create(void);
void cwist_sstring_destroy(cwist_sstring *str);

/** @name String manipulation API */
/** @{ */

/**
 * @brief Append raw bytes with length.
 */
cwist_error_t cwist_sstring_append_len(cwist_sstring *str, const char *data, size_t len);

/**
 * @brief Assign raw bytes with length.
 */
cwist_error_t cwist_sstring_assign_len(cwist_sstring *str, const char *data, size_t len);

/**
 * @brief Borrow an external buffer without copying; detached to heap on first mutation.
 */
cwist_error_t cwist_sstring_borrow(cwist_sstring *str, const char *data, size_t len);

/**
 * @brief Adopt a cwist_alloc'd heap buffer, taking ownership without copying.
 */
cwist_error_t cwist_sstring_adopt_len(cwist_sstring *str, char *buf, size_t len);

/**
 * @brief Initialize an sstring.
 */
cwist_error_t cwist_sstring_init (cwist_sstring *str);

/**
 * @brief Left-trim whitespace.
 */
cwist_error_t cwist_sstring_ltrim(cwist_sstring *str);

/**
 * @brief Right-trim whitespace.
 */
cwist_error_t cwist_sstring_rtrim(cwist_sstring *str);

/**
 * @brief Trim whitespace from both ends.
 */
cwist_error_t cwist_sstring_trim(cwist_sstring *str);

/**
 * @brief Change the internal buffer size.
 */
cwist_error_t cwist_sstring_change_size(cwist_sstring *str, size_t size, bool blow_data);

/**
 * @brief Assign a C string.
 */
cwist_error_t cwist_sstring_assign(cwist_sstring *str, char *data);

/**
 * @brief Append a C string.
 */
cwist_error_t cwist_sstring_append(cwist_sstring *str, const char *data);

/**
 * @brief Append another sstring.
 */
cwist_error_t cwist_sstring_append_sstring(cwist_sstring *str, const cwist_sstring *from);

/**
 * @brief Seek a substring at a location.
 */
cwist_error_t cwist_sstring_seek(cwist_sstring *str, char *substr, int location);

/**
 * @brief Copy to a C string buffer.
 */
cwist_error_t cwist_sstring_copy(cwist_sstring *origin, char *destination);

/**
 * @brief Copy from another sstring.
 */
cwist_error_t cwist_sstring_copy_sstring(cwist_sstring *origin, const cwist_sstring *from);

/**
 * @brief Compare with a C string.
 */
int cwist_sstring_compare(cwist_sstring *str, const char *compare_to);

/**
 * @brief Compare two sstrings.
 */
int cwist_sstring_compare_sstring(cwist_sstring *left, const cwist_sstring *right);

/**
 * @brief Get the current size.
 */
size_t cwist_sstring_get_size(cwist_sstring *str);

/**
 * @brief Extract a substring.
 */
cwist_sstring *cwist_sstring_substr(cwist_sstring *str, int start, int length);

/** @} */

enum cwist_sstring_error_t {
  ERR_SSTRING_OKAY,
  ERR_SSTRING_ZERO_LENGTH,
  ERR_SSTRING_NULL_STRING,
  ERR_SSTRING_CONSTANT,
  ERR_SSTRING_RESIZE_TOO_SMALL,
  ERR_SSTRING_RESIZE_TOO_LARGE,
  ERR_SSTRING_OUTOFBOUND,
};

#endif
