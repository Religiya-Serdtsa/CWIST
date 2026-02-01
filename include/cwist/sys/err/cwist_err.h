#ifndef __CWIST_ERR_H__
#define __CWIST_ERR_H__

#include <stdint.h>
#include <cjson/cJSON.h>

struct cwist_sstring;

typedef enum cwist_errtype_t {
  /// Signed 8-bit errcodes.
  CWIST_ERR_INT8, ///< Mostly used to check a char.
  /// Signed 16-bit errcodes.
  CWIST_ERR_INT16, ///< Used when checking common errcodes in Unix/Linux.
  CWIST_ERR_INT32,
  /// Big, signed errcodes (mostly unused).
  CWIST_ERR_INT64,
# if defined(__clang__) || defined(__GNUC__) && defined(USE_128BIT_ERRCODE)
  CWIST_ERR_INT128,
#endif
  /// Unsigned 8-bit errcodes.
  CWIST_ERR_UINT8, ///< Mostly used as 'byte'.
  CWIST_ERR_UINT16,
  CWIST_ERR_UINT32,
  /// Big, unsigned errcodes (mostly unused).
  CWIST_ERR_UINT64,
# if defined(__clang__) || defined(__GNUC__) && defined(USE_128BIT_ERRCODE)
  CWIST_ERR_UINT128,
#endif

  /// String types.
  CWIST_ERR_STRING,
  CWIST_ERR_JSON,
  /// Floating point types (mostly unused).
  CWIST_ERR_FLOAT,
  CWIST_ERR_DOUBLE,
} cwist_errtype_t;

typedef struct __prim_cwist_error_t {
  /**
   * @brief Signed error representations for internal errcodes.
   * User-oriented errors are beautified as JSON.
   */
  int8_t   err_i8;
  int16_t  err_i16;
  int32_t  err_i32;
  int64_t  err_i64;
#if (defined(__clang__) || defined(__GNUC__)) && defined(USE_128BIT_ERRCODE)
  int64_t err_i128;
#endif

  /**
   * @brief Unsigned error types, primarily for raw byte handling.
   * These errors are rare in modern web development.
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

/** @brief Functions */
cwist_error_t make_error(cwist_errtype_t type);
#endif
