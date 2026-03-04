#ifndef __CWIST_ZOD_H__
#define __CWIST_ZOD_H__

/**
 * @file zod.h
 * @brief Strict JSON schema validation (Zod-style) for CWIST.
 *
 * Unlike the self-healing layer, this module **rejects** data that does not
 * conform to the schema.  Use it at trust boundaries (HTTP API inputs, DB
 * writes) where silent coercion is undesirable.
 *
 * It shares the cwist_schema_t / cwist_schema_field_t types defined in
 * json_heal.h so the same schema descriptor can drive both healing and
 * strict validation.
 *
 * Usage:
 * @code
 * cwist_zod_result_t r = cwist_zod_parse(raw_body, &my_schema, &parsed);
 * if (!r.valid) {
 *     cwist_zod_print_errors(&r);
 *     // respond 400
 * }
 * // use parsed …
 * cJSON_Delete(parsed);
 * @endcode
 */

#include <cwist/core/utils/json_heal.h>   /* re-uses cwist_schema_t */
#include <cjson/cJSON.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Result types
 * ---------------------------------------------------------------------- */

#define CWIST_ZOD_MAX_ERRORS 32
#define CWIST_ZOD_FIELD_MAX  64
#define CWIST_ZOD_MSG_MAX    256

/** @brief A single validation failure. */
typedef struct cwist_zod_error {
    char field[CWIST_ZOD_FIELD_MAX];
    char message[CWIST_ZOD_MSG_MAX];
} cwist_zod_error_t;

/** @brief Result returned by cwist_zod_validate() / cwist_zod_parse(). */
typedef struct cwist_zod_result {
    bool              valid;
    cwist_zod_error_t errors[CWIST_ZOD_MAX_ERRORS];
    int               error_count;
} cwist_zod_result_t;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * @brief Strictly validate an already-parsed cJSON object against a schema.
 *
 * Checks:
 *  - All required fields are present.
 *  - All fields that appear in the schema have the declared type.
 *
 * No type coercion is performed; use cwist_json_heal() first if coercion
 * is acceptable.
 *
 * @param json    cJSON object (not array / primitive).
 * @param schema  Schema descriptor.
 * @return        Validation result.  result.valid == true iff all checks pass.
 */
cwist_zod_result_t cwist_zod_validate(const cJSON *json, const cwist_schema_t *schema);

/**
 * @brief Parse a raw JSON string and validate it in one step.
 *
 * On success *out is set to the parsed cJSON tree; the caller must call
 * cJSON_Delete(*out) when done.  On failure *out is set to NULL.
 *
 * @param raw     Null-terminated JSON string.
 * @param schema  Schema descriptor.
 * @param out     [out] Parsed cJSON tree on success, NULL otherwise.
 * @return        Validation result.  Check result.valid AND *out != NULL.
 */
cwist_zod_result_t cwist_zod_parse(const char *raw, const cwist_schema_t *schema,
                                    cJSON **out);

/**
 * @brief Print all validation errors to stderr.
 */
void cwist_zod_print_errors(const cwist_zod_result_t *r);

#endif /* __CWIST_ZOD_H__ */
