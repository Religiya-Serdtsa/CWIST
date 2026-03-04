#ifndef __CWIST_JSON_HEAL_H__
#define __CWIST_JSON_HEAL_H__

/**
 * @file json_heal.h
 * @brief Self-Healing JSON Recovery Layer (CWIST)
 *
 * Three-tier recovery pipeline:
 *  L1 – Syntax  : fast fixes (BOM, line-comments, trailing commas, bracket balancing)
 *  L2 – Schema  : field-name alias resolution + type coercion against a known schema
 *  L3 – SLLM    : user-supplied deep-recovery callback (AI / LLM inference)
 *
 * Usage:
 * @code
 * static const cwist_schema_field_t fields[] = {
 *     { "user_id", {"userId", "uid", NULL}, CWIST_FIELD_INT,    true  },
 *     { "name",    {NULL},                  CWIST_FIELD_STRING, true  },
 *     { "active",  {"is_active", NULL},     CWIST_FIELD_BOOL,   false },
 * };
 * static const cwist_schema_t schema = { fields, 3 };
 *
 * cwist_heal_config_t cfg = { .threshold = 0.8, .schema = &schema };
 * cwist_heal_result_t r   = cwist_json_heal(broken_input, &cfg);
 * if (r.json) {
 *     printf("healed(%d): %s\n  log: %s\n", r.level, r.json, r.log);
 * }
 * cwist_heal_result_free(&r);
 * @endcode
 */

#include <stdbool.h>
#include <stddef.h>
#include <cjson/cJSON.h>

/* -------------------------------------------------------------------------
 * Schema descriptor  (shared with zod.h)
 * ---------------------------------------------------------------------- */

/** @brief Expected value type for a schema field. */
typedef enum {
    CWIST_FIELD_STRING = 0,
    CWIST_FIELD_INT,
    CWIST_FIELD_FLOAT,
    CWIST_FIELD_BOOL,
    CWIST_FIELD_OBJECT,
    CWIST_FIELD_ARRAY,
} cwist_field_type_t;

#define CWIST_SCHEMA_MAX_ALIASES 8

/**
 * @brief Single field descriptor used by both the healer and the Zod validator.
 *
 * `aliases` is a NULL-terminated list of alternative names that will be
 * renamed to `name` during L2 alignment.  Fuzzy (case-insensitive,
 * separator-stripped) matching is applied when exact aliases do not match.
 */
typedef struct cwist_schema_field {
    const char        *name;                             ///< Canonical field name.
    const char        *aliases[CWIST_SCHEMA_MAX_ALIASES];///< Alternative names (NULL-terminated).
    cwist_field_type_t type;                             ///< Expected value type.
    bool               required;                         ///< Must be present?
} cwist_schema_field_t;

/** @brief A complete schema: array of field descriptors + count. */
typedef struct cwist_schema {
    const cwist_schema_field_t *fields;
    int                         field_count;
} cwist_schema_t;

/* -------------------------------------------------------------------------
 * L3 SLLM callback
 * ---------------------------------------------------------------------- */

/**
 * @brief Optional deep-recovery callback (e.g. backed by an LLM).
 *
 * Called only when L1 and L2 both fail to produce valid JSON.
 * Must return a heap-allocated (plain `malloc`) JSON string, or NULL on
 * failure.  The caller will free the returned string with `free()`.
 * The callback MUST use the standard C `malloc` allocator (not cwist_alloc)
 * so that the framework can release it with `free()`.
 *
 * @param broken_json  The damaged input string.
 * @param schema       Schema hint (may be NULL).
 * @param userdata     Opaque pointer from cwist_heal_config_t.
 * @return Heap-allocated recovered JSON string, or NULL.
 */
typedef char *(*cwist_sllm_heal_fn)(const char        *broken_json,
                                     const cwist_schema_t *schema,
                                     void              *userdata);

/* -------------------------------------------------------------------------
 * Healing configuration
 * ---------------------------------------------------------------------- */

/** @brief Configuration passed to cwist_json_heal(). */
typedef struct cwist_heal_config {
    double               threshold;     ///< Min confidence (0.0–1.0) to accept recovery. Default: 0.8.
    const cwist_schema_t *schema;       ///< Schema for L2 alignment (may be NULL).
    cwist_sllm_heal_fn   sllm_fn;       ///< Optional L3 deep-recovery callback.
    void                *sllm_userdata; ///< Passed verbatim to sllm_fn.
} cwist_heal_config_t;

/* -------------------------------------------------------------------------
 * Result
 * ---------------------------------------------------------------------- */

#define CWIST_HEAL_LOG_MAX 512

/** @brief Result of a healing attempt. */
typedef struct cwist_heal_result {
    char   *json;                    ///< Healed JSON string (free with cwist_heal_result_free). NULL on failure.
    bool    healed;                  ///< True if any modification was made.
    int     level;                   ///< Recovery level reached: 0=none, 1=L1, 2=L2, 3=L3.
    double  confidence;              ///< Estimated confidence in the recovery (0.0–1.0).
    char    log[CWIST_HEAL_LOG_MAX]; ///< Human-readable description of changes made.
} cwist_heal_result_t;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * @brief Attempt to heal a potentially broken JSON string.
 *
 * Tries L1 → L2 → L3 in order, stopping at the first successful parse whose
 * confidence meets the threshold.  If the input is already valid JSON it is
 * returned verbatim (healed=false), though L2 alignment is still applied when
 * a schema is provided.
 *
 * @param input  Raw (possibly broken) JSON string.
 * @param cfg    Healing configuration (may be NULL for defaults).
 * @return       Heap-allocated result; caller must call cwist_heal_result_free().
 */
cwist_heal_result_t cwist_json_heal(const char *input, const cwist_heal_config_t *cfg);

/**
 * @brief Apply L2 schema alignment to an already-parsed cJSON object in place.
 *
 * Renames aliased / fuzzy-matched field names to their canonical forms and
 * coerces mismatched types (e.g. `"123"` → `123` for an INT field).
 *
 * @param obj     cJSON object to modify in place.
 * @param schema  Schema descriptor.
 * @param log     Optional output buffer for change description.
 * @param log_sz  Size of log buffer.
 * @return        Number of fields modified, or -1 on error.
 */
int cwist_json_schema_align(cJSON *obj, const cwist_schema_t *schema,
                             char *log, size_t log_sz);

/**
 * @brief Free resources owned by a cwist_heal_result_t.
 */
void cwist_heal_result_free(cwist_heal_result_t *r);

#endif /* __CWIST_JSON_HEAL_H__ */
