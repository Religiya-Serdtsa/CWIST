#ifndef __CWIST_ERR_H__
#define __CWIST_ERR_H__

#include <stdint.h>
#include <cjson/cJSON.h>

struct cwist_sstring;

typedef enum cwist_errtype_t {
  // signed int errcodes
  CWIST_ERR_INT8, // mostly used to check a char
  CWIST_ERR_INT16, // used when checking common errcodes in Unix/Linux
  CWIST_ERR_INT32,
  // big, signed errcodes
  // WARN: mostly unused
  CWIST_ERR_INT64,
# if defined(__clang__) || defined(__GNUC__) && defined(USE_128BIT_ERRCODE)
  CWIST_ERR_INT128,
#endif
  // unsigned int errcodes
  CWIST_ERR_UINT8, // mostly used as 'byte'
  CWIST_ERR_UINT16,
  CWIST_ERR_UINT32,
  // big, unsigned errcodes
  // WARN: mostly unused
  CWIST_ERR_UINT64,
# if defined(__clang__) || defined(__GNUC__) && defined(USE_128BIT_ERRCODE)
  CWIST_ERR_UINT128,
#endif

  // string types
  CWIST_ERR_STRING,
  CWIST_ERR_JSON,
  // float types
  // WARN: mostly unused
  CWIST_ERR_FLOAT,
  CWIST_ERR_DOUBLE,
} cwist_errtype_t;

typedef struct __prim_cwist_error_t {
  /* These types are widely used when handling internal errcodes.
   * User-oriented errors are beautified as JSON.
   */
  int8_t   err_i8;
  int16_t  err_i16;
  int32_t  err_i32;
  int64_t  err_i64;
#if (defined(__clang__) || defined(__GNUC__)) && defined(USE_128BIT_ERRCODE)
  int64_t err_i128;
#endif

  /* Unsigned error types. These types are often utilised when handling raw bytes;
   * in many cases, these kinds of errors are hard to find on modern web development.
   */
  uint8_t   err_u8;
  uint16_t  err_u16;
  uint32_t  err_u32;
  uint64_t  err_u64;

#if (defined(__clang__) || defined(__GNUC__)) && defined(USE_128BIT_ERRCODE)
  int64_t err_i128;
#endif
  struct cwist_sstring *err_string;
  cJSON       *err_json;

} __prim_cwist_error_t;

typedef struct cwist_error_t {
  cwist_errtype_t errtype;
__prim_cwist_error_t error;
} cwist_error_t;

/* FUNCITONS */
cwist_error_t make_error(cwist_errtype_t type);
#endif
