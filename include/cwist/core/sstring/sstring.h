#ifndef __CWIST_SSTRING_H__
#define __CWIST_SSTRING_H__

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <cwist/sys/err/cwist_err.h>

typedef struct cwist_sstring {
  char   *data;  // please access this data if raw handling is necessary
  bool   is_fixed;
  size_t size;
  size_t (*get_size)(struct cwist_sstring *str);
  int    (*compare )(struct cwist_sstring *left, const struct cwist_sstring *right); // should mimic strcmp, internally use strncmp
  cwist_error_t (*copy  )(struct cwist_sstring *str, const struct cwist_sstring *from);
  cwist_error_t (*append)(struct cwist_sstring *str, const struct cwist_sstring *from);
                                   // returns 1 on success, returns 0 on failure
                                   // should be used in this form:
                                   // cwist_sstring str1;
                                   // cwist_sstring str2;
                                   // cwist_sstring_init(&str);
                                   // cwist_sstring_init(&str2);
                                   // cwist_error_t err = str1.copy(&str1, &str2);
                                   // cwist_error_t err = str2.append(&str2, &str1);
                                   // ...
} cwist_sstring;

cwist_sstring *cwist_sstring_create(void);
void cwist_sstring_destroy(cwist_sstring *str);

// String manipulation API
cwist_error_t cwist_sstring_append_len(cwist_sstring *str, const char *data, size_t len);
cwist_error_t cwist_sstring_assign_len(cwist_sstring *str, const char *data, size_t len);
cwist_error_t cwist_sstring_init (cwist_sstring *str);
cwist_error_t cwist_sstring_ltrim(cwist_sstring *str);
cwist_error_t cwist_sstring_rtrim(cwist_sstring *str);
cwist_error_t cwist_sstring_trim(cwist_sstring *str);
cwist_error_t cwist_sstring_change_size(cwist_sstring *str, size_t size, bool blow_data);
cwist_error_t cwist_sstring_assign(cwist_sstring *str, char *data);
cwist_error_t cwist_sstring_append(cwist_sstring *str, const char *data);
cwist_error_t cwist_sstring_append_sstring(cwist_sstring *str, const cwist_sstring *from);
cwist_error_t cwist_sstring_seek(cwist_sstring *str, char *substr, int location);
cwist_error_t cwist_sstring_copy(cwist_sstring *origin, char *destination);
cwist_error_t cwist_sstring_copy_sstring(cwist_sstring *origin, const cwist_sstring *from);
int cwist_sstring_compare(cwist_sstring *str, const char *compare_to);
int cwist_sstring_compare_sstring(cwist_sstring *left, const cwist_sstring *right);
size_t cwist_sstring_get_size(cwist_sstring *str);
cwist_sstring *cwist_sstring_substr(cwist_sstring *str, int start, int length);

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
