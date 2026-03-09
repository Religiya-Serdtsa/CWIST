#include <cwist/core/utils/zod.h>
#include <stdio.h>
#include <string.h>

/**
 * @file zod.c
 * @brief Lightweight schema validation helpers for cJSON payloads.
 */

/**
 * @brief Convert an enum field type into the human-readable validation label.
 * @param t Schema field type to stringify.
 * @return Static string describing the expected JSON type.
 */
static const char *field_type_name(cwist_field_type_t t) {
    switch (t) {
        case CWIST_FIELD_STRING: return "string";
        case CWIST_FIELD_INT:    return "int";
        case CWIST_FIELD_FLOAT:  return "float";
        case CWIST_FIELD_BOOL:   return "bool";
        case CWIST_FIELD_OBJECT: return "object";
        case CWIST_FIELD_ARRAY:  return "array";
        default:                 return "unknown";
    }
}

/**
 * @brief Check whether a cJSON item matches the requested schema type.
 * @param item JSON node being validated.
 * @param t Expected CWIST field type.
 * @return true when the node satisfies the schema type.
 */
static bool type_matches(const cJSON *item, cwist_field_type_t t) {
    switch (t) {
        case CWIST_FIELD_STRING: return cJSON_IsString(item);
        case CWIST_FIELD_INT:
        case CWIST_FIELD_FLOAT:  return cJSON_IsNumber(item);
        case CWIST_FIELD_BOOL:   return cJSON_IsBool(item);
        case CWIST_FIELD_OBJECT: return cJSON_IsObject(item);
        case CWIST_FIELD_ARRAY:  return cJSON_IsArray(item);
        default:                 return false;
    }
}

/**
 * @brief Append one validation error to the result accumulator.
 * @param r Validation result being populated.
 * @param field Field name associated with the error.
 * @param msg Human-readable validation message.
 */
static void add_error(cwist_zod_result_t *r, const char *field, const char *msg) {
    if (r->error_count >= CWIST_ZOD_MAX_ERRORS) return;
    cwist_zod_error_t *e = &r->errors[r->error_count++];
    snprintf(e->field,   CWIST_ZOD_FIELD_MAX, "%s", field);
    snprintf(e->message, CWIST_ZOD_MSG_MAX,   "%s", msg);
    r->valid = false;
}

/**
 * @brief Validate a parsed JSON object against a flat CWIST schema.
 * @param json Parsed JSON object to inspect.
 * @param schema Expected field list and type requirements.
 * @return Validation result containing success flag and collected errors.
 */
cwist_zod_result_t cwist_zod_validate(const cJSON *json, const cwist_schema_t *schema) {
    cwist_zod_result_t r;
    memset(&r, 0, sizeof(r));
    r.valid = true;

    if (!json || !schema) {
        add_error(&r, "(root)", "null json or schema pointer");
        return r;
    }

    if (!cJSON_IsObject(json)) {
        add_error(&r, "(root)", "expected a JSON object at root");
        return r;
    }

    for (int fi = 0; fi < schema->field_count; fi++) {
        const cwist_schema_field_t *fd = &schema->fields[fi];
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, fd->name);

        /* Missing field */
        if (!item) {
            if (fd->required) {
                char msg[CWIST_ZOD_MSG_MAX];
                snprintf(msg, sizeof(msg),
                         "required field '%s' is missing", fd->name);
                add_error(&r, fd->name, msg);
            }
            continue;
        }

        /* Wrong type */
        if (!type_matches(item, fd->type)) {
            char msg[CWIST_ZOD_MSG_MAX];
            snprintf(msg, sizeof(msg),
                     "expected type '%s'", field_type_name(fd->type));
            add_error(&r, fd->name, msg);
        }
    }

    return r;
}

/**
 * @brief Parse raw JSON text and validate it against a CWIST schema.
 * @param raw Raw JSON text to parse.
 * @param schema Expected field list and type requirements.
 * @param out Optional output pointer that receives the parsed cJSON object on success.
 * @return Validation result containing success flag and collected errors.
 */
cwist_zod_result_t cwist_zod_parse(const char *raw, const cwist_schema_t *schema,
                                    cJSON **out) {
    cwist_zod_result_t r;
    memset(&r, 0, sizeof(r));
    r.valid = false;

    if (out) *out = NULL;

    if (!raw) {
        add_error(&r, "(root)", "null input string");
        return r;
    }

    cJSON *parsed = cJSON_Parse(raw);
    if (!parsed) {
        add_error(&r, "(root)", "JSON parse error: invalid syntax");
        return r;
    }

    r = cwist_zod_validate(parsed, schema);
    if (r.valid) {
        if (out) *out = parsed;
        else     cJSON_Delete(parsed);
    } else {
        cJSON_Delete(parsed);
    }
    return r;
}

/**
 * @brief Print validation errors to stderr using the built-in ZOD-like format.
 * @param r Validation result to print.
 */
void cwist_zod_print_errors(const cwist_zod_result_t *r) {
    if (!r) return;
    for (int i = 0; i < r->error_count; i++) {
        fprintf(stderr, "[ZOD] %s: %s\n",
                r->errors[i].field, r->errors[i].message);
    }
}
