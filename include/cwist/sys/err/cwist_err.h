/**
 * @file cwist_err.h
 * @brief Unified error code and type definitions for the CWIST system layer.
 * @author Lee Yunjin
 * @date 2026-04-27
 */

#ifndef __CWIST_ERR_H__
#define __CWIST_ERR_H__

#include <stdint.h>
#include <stdbool.h>
#include <cjson/cJSON.h>

/** @struct cwist_sstring */
struct cwist_sstring;

/**
 * @enum cwist_errtype_t
 * @brief Enumeration to identify the physical type of error data.
 */
typedef enum cwist_errtype_t {
    /* --- Signed Integer Types --- */
    CWIST_ERR_INT8,   /**< 8-bit signed error (mostly for char checks) */
    CWIST_ERR_INT16,  /**< 16-bit signed error (Unix/Linux errno compatibility) */
    CWIST_ERR_INT32,  /**< 32-bit signed error (Standard function returns) */
    CWIST_ERR_INT64,  /**< 64-bit signed error */
#if (defined(__clang__) || defined(__GNUC__)) && defined(USE_128BIT_ERRCODE)
    CWIST_ERR_INT128, /**< 128-bit signed error (if supported by compiler) */
#endif

    /* --- Unsigned Integer Types --- */
    CWIST_ERR_UINT8,  /**< 8-bit unsigned error (Raw byte handling) */
    CWIST_ERR_UINT16, /**< 16-bit unsigned error */
    CWIST_ERR_UINT32, /**< 32-bit unsigned error */
    CWIST_ERR_UINT64, /**< 64-bit unsigned error */
#if (defined(__clang__) || defined(__GNUC__)) && defined(USE_128BIT_ERRCODE)
    CWIST_ERR_UINT128, /**< 128-bit unsigned error */
#endif

    /* --- Complex Types --- */
    CWIST_ERR_STRING, /**< Error message in string format */
    CWIST_ERR_JSON,   /**< Detailed error data in cJSON format */
    CWIST_ERR_FLOAT,  /**< Single-precision floating point error data */
    CWIST_ERR_DOUBLE, /**< Double-precision floating point error data */
} cwist_errtype_t;

/**
 * @struct __prim_cwist_error_t
 * @brief Internal union-like structure to store the actual error value.
 */
typedef struct __prim_cwist_error_t {
    /* Signed representations */
    int8_t   err_i8;   /**< 8-bit signed error value */
    int16_t  err_i16;  /**< 16-bit signed error value */
    int32_t  err_i32;  /**< 32-bit signed error value */
    int64_t  err_i64;  /**< 64-bit signed error value */
#if (defined(__clang__) || defined(__GNUC__)) && defined(USE_128BIT_ERRCODE)
    __int128 err_i128; /**< 128-bit signed error value */
#endif

    /* Unsigned representations */
    uint8_t  err_u8;   /**< 8-bit unsigned error value */
    uint16_t err_u16;  /**< 16-bit unsigned error value */
    uint32_t err_u32;  /**< 32-bit unsigned error value */
    uint64_t err_u64;  /**< 64-bit unsigned error value */
#if (defined(__clang__) || defined(__GNUC__)) && defined(USE_128BIT_ERRCODE)
    unsigned __int128 err_u128; /**< 128-bit unsigned error value */
#endif

    struct cwist_sstring *err_string; /**< Pointer to sstring error object */
    cJSON *err_json;                  /**< Pointer to cJSON error object */
} __prim_cwist_error_t;

/**
 * @struct cwist_error_t
 * @brief Integrated error structure used throughout the system.
 */
typedef struct cwist_error_t {
    cwist_errtype_t errtype;      /**< Discriminator for the error value type */
    __prim_cwist_error_t error;   /**< The actual error data value */
} cwist_error_t;

/**
 * @brief Test whether an error object represents success.
 *
 * Callers must use this instead of open-coding
 * `err.errtype != CWIST_ERR_YY || err.error.err_ZZ != 0`: guessing the wrong
 * channel reads the wrong union member and has repeatedly misreported
 * success as failure (e.g. an INT8-channel function checked as INT16).
 *
 * Success means the active channel holds its zero/empty value.
 */
static inline bool cwist_error_is_ok(const cwist_error_t *err) {
    if (!err) return true;
    switch (err->errtype) {
        case CWIST_ERR_INT8:   return err->error.err_i8 == 0;
        case CWIST_ERR_INT16:  return err->error.err_i16 == 0;
        case CWIST_ERR_INT32:  return err->error.err_i32 == 0;
        case CWIST_ERR_INT64:  return err->error.err_i64 == 0;
#if (defined(__clang__) || defined(__GNUC__)) && defined(USE_128BIT_ERRCODE)
        case CWIST_ERR_INT128: return err->error.err_i128 == 0;
#endif
        case CWIST_ERR_UINT8:  return err->error.err_u8 == 0;
        case CWIST_ERR_UINT16: return err->error.err_u16 == 0;
        case CWIST_ERR_UINT32: return err->error.err_u32 == 0;
        case CWIST_ERR_UINT64: return err->error.err_u64 == 0;
#if (defined(__clang__) || defined(__GNUC__)) && defined(USE_128BIT_ERRCODE)
        case CWIST_ERR_UINT128: return err->error.err_u128 == 0;
#endif
        case CWIST_ERR_STRING: return err->error.err_string == NULL;
        case CWIST_ERR_JSON:   return err->error.err_json == NULL;
        default:               return false;
    }
}

/* --- Function Prototypes --- */

/**
 * @brief Creates an error object initialized with the specified type.
 * @param type The type of error to create (cwist_errtype_t)
 * @return Initialized cwist_error_t structure
 */
cwist_error_t make_error(cwist_errtype_t type);

/**
 * @brief Release resources owned by an error value.
 * Currently only CWIST_ERR_JSON owns memory (the cJSON payload); all other
 * variants are no-ops. Safe on success values and on NULL.
 */
void cwist_error_dispose(cwist_error_t *err);

/* --- Generic Error Macros --- */

/** @name Common Status Codes
 * @{
 */
#define CWIST_SUCCESS                0  /**< Success (Standard OK) */
#define CWIST_FAILURE               -1  /**< Generic failure */
#define CWIST_ERROR_IO              -2  /**< I/O or Network level error (inc. SSL) */
#define CWIST_ERROR_NOMEM           -3  /**< Memory allocation or resource exhaustion */
#define CWIST_ERROR_INVALID_PARAM   -4  /**< Invalid argument passed to function */
#define CWIST_ERROR_PROTOCOL        -5  /**< Protocol-specific violation (HTTP/2, etc.) */
#define CWIST_ERROR_TIMEOUT         -6  /**< Operation timed out */
/** @} */

#endif /* __CWIST_ERR_H__ */
