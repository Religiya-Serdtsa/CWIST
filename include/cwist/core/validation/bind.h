/**
 * @file bind.h
 * @brief Declarative Schema Validation and Type-Safe Binding for CWIST.
 *
 * Provides a _Generic-driven dispatch layer that maps JSON/Form payloads
 * directly into user-defined C structures with zero-copy semantics where
 * possible.  Validation rules (REQUIRED, MIN/MAX, REGEX, EMAIL) are
 * evaluated at bind-time and automatically produce 400 Bad Request
 * responses with descriptive JSON error payloads on failure.
 */

#ifndef __CWIST_VALIDATION_BIND_H__
#define __CWIST_VALIDATION_BIND_H__

#include <cwist/net/http/http.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/utils/json_builder.h>
#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <regex.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Validation rule descriptors
 * ---------------------------------------------------------------------- */

/** @brief Supported validation constraint types. */
typedef enum cwist_bind_rule_type {
    CWIST_BIND_RULE_REQUIRED,   ///< Field must be present and non-empty.
    CWIST_BIND_RULE_MIN_LEN,    ///< Minimum string length.
    CWIST_BIND_RULE_MAX_LEN,    ///< Maximum string length.
    CWIST_BIND_RULE_MIN_VAL,    ///< Minimum numeric value.
    CWIST_BIND_RULE_MAX_VAL,    ///< Maximum numeric value.
    CWIST_BIND_RULE_REGEX,      ///< Must match POSIX extended regular expression.
    CWIST_BIND_RULE_EMAIL,      ///< Must conform to RFC 5322 simplified email shape.
    CWIST_BIND_RULE_CUSTOM,     ///< User-supplied predicate.
} cwist_bind_rule_type_t;

/** @brief Single validation rule attached to a field. */
typedef struct cwist_bind_rule {
    cwist_bind_rule_type_t type;
    union {
        size_t      min_len;    ///< For MIN_LEN.
        size_t      max_len;    ///< For MAX_LEN.
        long double min_val;    ///< For MIN_VAL.
        long double max_val;    ///< For MAX_VAL.
        const char *pattern;    ///< For REGEX (static string, compiled once).
        struct {
            bool (*fn)(const char *value, size_t len, void *ctx);
            void *ctx;
        } custom;               ///< For CUSTOM.
    } u;
    const char *error_message;  ///< Optional override for the default error text.
} cwist_bind_rule_t;

/** @brief Logical type of a bind target field. */
typedef enum cwist_bind_field_type {
    CWIST_BIND_BOOL,
    CWIST_BIND_INT,
    CWIST_BIND_UINT,
    CWIST_BIND_FLOAT,
    CWIST_BIND_DOUBLE,
    CWIST_BIND_STRING,      ///< Null-terminated C string (copied).
    CWIST_BIND_SSTRING,     ///< cwist_sstring target.
    CWIST_BIND_JSON_OBJECT, ///< cJSON * (ownership transferred to caller).
} cwist_bind_field_type_t;

/** @brief Per-field binding descriptor. */
typedef struct cwist_bind_field {
    const char              *json_key;      ///< Source key in JSON/Form payload.
    cwist_bind_field_type_t  target_type;   ///< Destination C type.
    size_t                   target_offset; ///< Byte offset inside the user struct.
    size_t                   target_size;   ///< sizeof(destination) for bounds checking.
    const cwist_bind_rule_t *rules;         ///< Array of rules (NULL-terminated by type == -1).
    size_t                   rule_count;    ///< Number of rules (optional, for static arrays).
} cwist_bind_field_t;

/** @brief Complete schema descriptor for a struct. */
typedef struct cwist_bind_schema {
    const cwist_bind_field_t *fields;
    size_t                    field_count;
    size_t                    struct_size;  ///< sizeof(user_struct) for bounds checking.
} cwist_bind_schema_t;

/* -------------------------------------------------------------------------
 * Result types
 * ---------------------------------------------------------------------- */

#define CWIST_BIND_MAX_ERRORS 32
#define CWIST_BIND_MSG_MAX    256
#define CWIST_BIND_KEY_MAX    64

/** @brief A single bind/validation failure. */
typedef struct cwist_bind_error {
    char key[CWIST_BIND_KEY_MAX];
    char message[CWIST_BIND_MSG_MAX];
} cwist_bind_error_t;

/** @brief Result of a bind operation. */
typedef struct cwist_bind_result {
    bool               ok;
    cwist_bind_error_t errors[CWIST_BIND_MAX_ERRORS];
    size_t             error_count;
} cwist_bind_result_t;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * @brief Bind a JSON payload to a user struct according to a schema.
 *
 * The JSON body of @p req is parsed once and then each field in @p schema
 * is extracted, validated, and written into @p out at the specified offset.
 *
 * On validation failure the function returns false and populates
 * @p result with per-field error messages.  The caller can then pass
 * @p result to cwist_bind_write_error_response() to emit a 400 response.
 *
 * @param req    HTTP request whose body contains the JSON payload.
 * @param schema Bind schema describing the target struct layout.
 * @param out    Pointer to the user struct to populate.
 * @param result [out] Detailed validation result.
 * @return true when every field passes validation and the struct is filled.
 */
bool cwist_app_req_bind_json(cwist_http_request *req,
                              const cwist_bind_schema_t *schema,
                              void *out,
                              cwist_bind_result_t *result);

/**
 * @brief Bind URL-encoded form data to a user struct.
 *
 * Operates on req->body treating it as application/x-www-form-urlencoded.
 *
 * @param req    HTTP request whose body contains the form payload.
 * @param schema Bind schema describing the target struct layout.
 * @param out    Pointer to the user struct to populate.
 * @param result [out] Detailed validation result.
 * @return true when every field passes validation and the struct is filled.
 */
bool cwist_app_req_bind_form(cwist_http_request *req,
                              const cwist_bind_schema_t *schema,
                              void *out,
                              cwist_bind_result_t *result);

/**
 * @brief Populate a response with 400 Bad Request and a JSON error payload.
 *
 * The payload follows the shape:
 * @code
 * {"success":false,"errors":[{"field":"name","message":"..."},...]}
 * @endcode
 *
 * @param res    Response object to populate.
 * @param result Validation result produced by a failed bind call.
 */
void cwist_bind_write_error_response(cwist_http_response *res,
                                      const cwist_bind_result_t *result);

/**
 * @brief Convenience wrapper: bind JSON and auto-respond 400 on failure.
 *
 * If binding succeeds, the function returns true and @p out is filled.
 * If binding fails, the function automatically sets res to 400 Bad Request
 * with a JSON error body and returns false.
 *
 * @param req    HTTP request.
 * @param res    HTTP response (may be mutated on failure).
 * @param schema Bind schema.
 * @param out    Pointer to the user struct to populate.
 * @return true on success; false when res has been prepped with 400 errors.
 */
bool cwist_app_req_bind_json_or_400(cwist_http_request *req,
                                     cwist_http_response *res,
                                     const cwist_bind_schema_t *schema,
                                     void *out);

/* -------------------------------------------------------------------------
 * _Generic type-safe dispatch macros
 * ---------------------------------------------------------------------- */

/**
 * @brief Internal helper that resolves the cwist_bind_field_type_t for a C expression.
 *
 * Uses C11 _Generic to map common C types to the binding enum.  This is the
 * core mechanism that makes the macro API type-safe.
 */
#define CWIST_BIND_TYPEOF(x) _Generic((x), \
    bool:               CWIST_BIND_BOOL,      \
    char *:             CWIST_BIND_STRING,    \
    const char *:       CWIST_BIND_STRING,    \
    int:                CWIST_BIND_INT,       \
    unsigned int:       CWIST_BIND_UINT,      \
    long:               CWIST_BIND_INT,       \
    unsigned long:      CWIST_BIND_UINT,      \
    long long:          CWIST_BIND_INT,       \
    unsigned long long: CWIST_BIND_UINT,      \
    float:              CWIST_BIND_FLOAT,     \
    double:             CWIST_BIND_DOUBLE,    \
    long double:        CWIST_BIND_DOUBLE,    \
    cwist_sstring *:    CWIST_BIND_SSTRING,   \
    cJSON *:            CWIST_BIND_JSON_OBJECT, \
    default:            CWIST_BIND_STRING     \
)

/**
 * @brief Compute the byte offset of a member inside a struct type.
 */
#define CWIST_BIND_OFFSET(st, m) offsetof(st, m)

/**
 * @brief Compute the size of a struct member.
 */
#define CWIST_BIND_SIZEOF(st, m) sizeof(((st *)0)->m)

/**
 * @brief Declare a static rule array suitable for use with CWIST_BIND_FIELD.
 *
 * @param name  C identifier for the array.
 * @param ...   cwist_bind_rule_t initialiser list.
 *
 * Example:
 * @code
 * CWIST_BIND_RULES(email_rules,
 *     CWIST_RULE_REQUIRED(),
 *     CWIST_RULE_EMAIL());
 * @endcode
 */
#define CWIST_BIND_RULES(name, ...) \
    static const cwist_bind_rule_t name[] = { __VA_ARGS__, {-1, {0}, NULL} }

/**
 * @brief Helper to declare a field descriptor with automatic type inference.
 *
 * @param StructType  The C struct type being bound (e.g., struct user).
 * @param MemberName  The member name inside the struct (e.g., email).
 * @param JsonKey     The JSON key that maps to this member.
 * @param RulesArray  A cwist_bind_rule_t array created with CWIST_BIND_RULES.
 *
 * Example:
 * @code
 * typedef struct { char email[128]; int age; } user_t;
 * CWIST_BIND_RULES(email_rules, CWIST_RULE_REQUIRED(), CWIST_RULE_EMAIL());
 * CWIST_BIND_RULES(age_rules,
 *     CWIST_RULE_MIN_VAL(18),
 *     CWIST_RULE_MAX_VAL(120));
 *
 * static const cwist_bind_field_t user_fields[] = {
 *     CWIST_BIND_FIELD(user_t, email, "email", email_rules),
 *     CWIST_BIND_FIELD(user_t, age,   "age",   age_rules),
 * };
 * @endcode
 */
#define CWIST_BIND_FIELD(StructType, MemberName, JsonKey, RulesArray) \
    { \
        .json_key      = (JsonKey), \
        .target_type   = CWIST_BIND_TYPEOF(((StructType *)0)->MemberName), \
        .target_offset = CWIST_BIND_OFFSET(StructType, MemberName), \
        .target_size   = CWIST_BIND_SIZEOF(StructType, MemberName), \
        .rules         = (RulesArray) \
    }

/**
 * @brief Build a schema descriptor from an array of field descriptors.
 */
#define CWIST_BIND_SCHEMA(StructType, FieldArray) \
    (cwist_bind_schema_t){ \
        .fields      = (FieldArray), \
        .field_count = sizeof(FieldArray) / sizeof((FieldArray)[0]), \
        .struct_size = sizeof(StructType) \
    }

/* -------------------------------------------------------------------------
 * Rule construction helpers
 * ---------------------------------------------------------------------- */

#define CWIST_RULE_REQUIRED() \
    (cwist_bind_rule_t){CWIST_BIND_RULE_REQUIRED, {0}, NULL}

#define CWIST_RULE_MIN_LEN(n) \
    (cwist_bind_rule_t){CWIST_BIND_RULE_MIN_LEN, {.min_len = (n)}, NULL}

#define CWIST_RULE_MAX_LEN(n) \
    (cwist_bind_rule_t){CWIST_BIND_RULE_MAX_LEN, {.max_len = (n)}, NULL}

#define CWIST_RULE_MIN_VAL(v) \
    (cwist_bind_rule_t){CWIST_BIND_RULE_MIN_VAL, {.min_val = (v)}, NULL}

#define CWIST_RULE_MAX_VAL(v) \
    (cwist_bind_rule_t){CWIST_BIND_RULE_MAX_VAL, {.max_val = (v)}, NULL}

#define CWIST_RULE_REGEX(pat) \
    (cwist_bind_rule_t){CWIST_BIND_RULE_REGEX, {.pattern = (pat)}, NULL}

#define CWIST_RULE_EMAIL() \
    (cwist_bind_rule_t){CWIST_BIND_RULE_EMAIL, {0}, NULL}

#define CWIST_RULE_CUSTOM(fn_ptr, ctx_ptr) \
    (cwist_bind_rule_t){CWIST_BIND_RULE_CUSTOM, {.custom = {.fn = (fn_ptr), .ctx = (ctx_ptr)}}, NULL}

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_VALIDATION_BIND_H__ */
