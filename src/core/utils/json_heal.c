#define _POSIX_C_SOURCE 200809L
#include <cwist/core/utils/json_heal.h>
#include <cwist/core/mem/alloc.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/**
 * @file json_heal.c
 * @brief Multi-stage JSON recovery pipeline with optional schema alignment.
 */

/* ==========================================================================
 * Internal dynamic string buffer (heap-backed, cwist_alloc / cwist_realloc)
 * ======================================================================== */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} strbuf_t;

static bool strbuf_init(strbuf_t *b, size_t init_cap) {
    b->data = (char *)cwist_alloc(init_cap + 1);
    if (!b->data) return false;
    b->len       = 0;
    b->cap       = init_cap;
    b->data[0]   = '\0';
    return true;
}

static bool strbuf_push(strbuf_t *b, char c) {
    if (b->len >= b->cap) {
        size_t new_cap = b->cap * 2 + 64;
        char  *nd      = (char *)cwist_realloc(b->data, new_cap + 1);
        if (!nd) return false;
        b->data = nd;
        b->cap  = new_cap;
    }
    b->data[b->len++] = c;
    b->data[b->len]   = '\0';
    return true;
}

static void strbuf_free(strbuf_t *b) {
    if (b->data) {
        cwist_free(b->data);
        b->data = NULL;
    }
    b->len = b->cap = 0;
}

/* ==========================================================================
 * Log helper
 * ======================================================================== */

static void log_append(char *log, size_t log_sz, const char *msg) {
    if (!log || log_sz == 0) return;
    size_t cur = strlen(log);
    if (cur >= log_sz - 1) return;
    strncat(log, msg, log_sz - cur - 1);
}

/* ==========================================================================
 * L1 – Syntax healing
 *
 * Steps applied in order:
 *  1. Strip UTF-8 BOM.
 *  2. Remove JavaScript-style // line-comments (not valid JSON but common).
 *  3. Fix trailing commas before ] or }.
 *  4. Balance unmatched { } [ ] by appending missing closers at the end.
 *  5. Attempt cJSON_Parse; return NULL if it still fails.
 * ======================================================================== */

/* Remove trailing commas: ",]" → "]"  /  ",}" → "}" */
static void remove_trailing_commas(strbuf_t *b) {
    char  *d   = b->data;
    size_t len = b->len;
    for (size_t i = 1; i < len; i++) {
        if (d[i] == ']' || d[i] == '}') {
            /* find last non-whitespace before i */
            size_t j = i;
            while (j > 0 && (d[j-1] == ' ' || d[j-1] == '\t' ||
                              d[j-1] == '\r' || d[j-1] == '\n')) {
                j--;
            }
            if (j > 0 && d[j-1] == ',') {
                memmove(&d[j-1], &d[j], len - j + 1);
                len--;
                b->len = len;
                i--; /* re-examine the same position */
            }
        }
    }
}

/* Append missing closing brackets / braces at the end of the buffer. */
static void balance_brackets(strbuf_t *b, char *log, size_t log_sz) {
    char  *stack  = (char *)cwist_alloc(b->len + 1);
    if (!stack) {
        log_append(log, log_sz, "[L1] bracket-balance skipped (alloc failure); ");
        return;
    }
    int    top       = 0;
    bool   in_string = false;
    bool   escaped   = false;

    const char *d   = b->data;
    size_t      len = b->len;

    for (size_t i = 0; i < len; i++) {
        char c = d[i];
        if (escaped)                  { escaped = false; continue; }
        if (c == '\\' && in_string)   { escaped = true;  continue; }
        if (c == '"')                 { in_string = !in_string; continue; }
        if (in_string)                continue;

        if      (c == '{')            stack[top++] = '}';
        else if (c == '[')            stack[top++] = ']';
        else if (c == '}' || c == ']') {
            if (top > 0 && stack[top-1] == c) top--;
            /* mismatched closer — ignore; cJSON will flag remaining issues */
        }
    }

    if (top > 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "[L1] appended %d missing closer(s); ", top);
        log_append(log, log_sz, msg);
    }

    while (top > 0) strbuf_push(b, stack[--top]);
    cwist_free(stack);
}

/* Returns a heap-allocated (cwist_alloc) fixed string, or NULL on failure. */
static char *l1_heal(const char *input, char *log, size_t log_sz) {
    if (!input) return NULL;
    size_t in_len = strlen(input);

    strbuf_t b;
    if (!strbuf_init(&b, in_len + 64)) return NULL;

    const char *p = input;

    /* 1. Strip UTF-8 BOM */
    if (in_len >= 3 &&
        (unsigned char)p[0] == 0xEF &&
        (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) {
        p += 3;
        log_append(log, log_sz, "[L1] stripped BOM; ");
    }

    /* 2. Copy, stripping // line-comments outside strings */
    bool in_str   = false;
    bool esc      = false;
    bool stripped = false;

    while (*p) {
        char c = *p;
        if (esc)                       { esc = false; strbuf_push(&b, c); p++; continue; }
        if (c == '\\' && in_str)       { esc = true;  strbuf_push(&b, c); p++; continue; }
        if (c == '"')                  { in_str = !in_str; strbuf_push(&b, c); p++; continue; }
        if (!in_str && c == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            stripped = true;
            continue;
        }
        strbuf_push(&b, c);
        p++;
    }
    if (stripped) log_append(log, log_sz, "[L1] removed line comment(s); ");

    /* 3. Fix trailing commas */
    size_t old_len = b.len;
    remove_trailing_commas(&b);
    if (b.len != old_len) log_append(log, log_sz, "[L1] removed trailing comma(s); ");

    /* 4. Balance brackets */
    balance_brackets(&b, log, log_sz);

    /* 5. Try to parse */
    cJSON *test = cJSON_Parse(b.data);
    if (!test) {
        strbuf_free(&b);
        return NULL;
    }
    cJSON_Delete(test);
    return b.data; /* caller owns (cwist_alloc backing) */
}

/* ==========================================================================
 * L2 – Schema alignment (field renaming + type coercion)
 * ======================================================================== */

/* Normalise: lower-case, strip _ and - for fuzzy comparison. */
static void normalise_name(const char *src, char *dst, size_t dst_sz) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_sz - 1; i++) {
        char c = src[i];
        if (c == '_' || c == '-') continue;
        dst[j++] = (char)tolower((unsigned char)c);
    }
    dst[j] = '\0';
}

static bool names_fuzzy_match(const char *a, const char *b) {
    char na[64], nb[64];
    normalise_name(a, na, sizeof(na));
    normalise_name(b, nb, sizeof(nb));
    return strcmp(na, nb) == 0;
}

/* Find a cJSON child whose key fuzzy-matches `name`.  NULL if not found. */
static cJSON *find_fuzzy(cJSON *obj, const char *name, const char **found_key) {
    cJSON *child = obj->child;
    while (child) {
        if (names_fuzzy_match(child->string, name)) {
            if (found_key) *found_key = child->string;
            return child;
        }
        child = child->next;
    }
    return NULL;
}

int cwist_json_schema_align(cJSON *obj, const cwist_schema_t *schema,
                             char *log, size_t log_sz) {
    if (!obj || !schema || !cJSON_IsObject(obj)) return -1;

    int changes = 0;

    for (int fi = 0; fi < schema->field_count; fi++) {
        const cwist_schema_field_t *fd = &schema->fields[fi];

        /* ---- Locate the field in the JSON object ---- */
        cJSON      *item     = NULL;
        const char *found_as = NULL;

        /* Try canonical name first */
        item = cJSON_GetObjectItemCaseSensitive(obj, fd->name);
        if (item) { found_as = fd->name; }

        /* Try explicit aliases */
        if (!item) {
            for (int ai = 0; ai < CWIST_SCHEMA_MAX_ALIASES; ai++) {
                if (!fd->aliases[ai]) break;
                item = cJSON_GetObjectItemCaseSensitive(obj, fd->aliases[ai]);
                if (item) { found_as = fd->aliases[ai]; break; }
                /* fuzzy alias match */
                const char *fk = NULL;
                item = find_fuzzy(obj, fd->aliases[ai], &fk);
                if (item) { found_as = fk; break; }
            }
        }

        /* Fuzzy match on canonical name */
        if (!item) {
            const char *fk = NULL;
            item = find_fuzzy(obj, fd->name, &fk);
            if (item) found_as = fk;
        }

        if (!item) continue;

        /* ---- Rename to canonical name if needed ---- */
        if (strcmp(found_as, fd->name) != 0) {
            cJSON *clone = cJSON_Duplicate(item, 1);
            cJSON_DeleteItemFromObjectCaseSensitive(obj, (char *)found_as);
            if (clone) {
                cJSON_AddItemToObject(obj, fd->name, clone);
                item = clone;
            }
            char msg[128];
            snprintf(msg, sizeof(msg), "[L2] '%s'→'%s'; ", found_as, fd->name);
            log_append(log, log_sz, msg);
            changes++;
        }

        /* ---- Type coercion ---- */
        bool type_ok;
        switch (fd->type) {
            case CWIST_FIELD_STRING: type_ok = cJSON_IsString(item); break;
            case CWIST_FIELD_INT:
            case CWIST_FIELD_FLOAT:  type_ok = cJSON_IsNumber(item); break;
            case CWIST_FIELD_BOOL:   type_ok = cJSON_IsBool(item);   break;
            case CWIST_FIELD_OBJECT: type_ok = cJSON_IsObject(item); break;
            case CWIST_FIELD_ARRAY:  type_ok = cJSON_IsArray(item);  break;
            default:                 type_ok = true;                 break;
        }

        if (type_ok) continue;

        char msg[128];

        /* String → Number */
        if ((fd->type == CWIST_FIELD_INT || fd->type == CWIST_FIELD_FLOAT)
            && cJSON_IsString(item)) {
            char *end = NULL;
            double v  = strtod(item->valuestring, &end);
            if (end && *end == '\0') {
                cJSON *num = cJSON_CreateNumber(v);
                if (num) {
                    cJSON_ReplaceItemInObjectCaseSensitive(obj, fd->name, num);
                    snprintf(msg, sizeof(msg), "[L2] '%s': str→num; ", fd->name);
                    log_append(log, log_sz, msg);
                    changes++;
                }
            }
        }
        /* Number → String */
        else if (fd->type == CWIST_FIELD_STRING && cJSON_IsNumber(item)) {
            char numstr[64];
            snprintf(numstr, sizeof(numstr), "%g", item->valuedouble);
            cJSON *str = cJSON_CreateString(numstr);
            if (str) {
                cJSON_ReplaceItemInObjectCaseSensitive(obj, fd->name, str);
                snprintf(msg, sizeof(msg), "[L2] '%s': num→str; ", fd->name);
                log_append(log, log_sz, msg);
                changes++;
            }
        }
        /* String "true"/"false"/"1"/"0" → Bool */
        else if (fd->type == CWIST_FIELD_BOOL && cJSON_IsString(item)) {
            int bval = -1;
            if (strcasecmp(item->valuestring, "true")  == 0 ||
                strcmp   (item->valuestring, "1")      == 0) bval = 1;
            if (strcasecmp(item->valuestring, "false") == 0 ||
                strcmp   (item->valuestring, "0")      == 0) bval = 0;
            if (bval >= 0) {
                cJSON *bj = bval ? cJSON_CreateTrue() : cJSON_CreateFalse();
                if (bj) {
                    cJSON_ReplaceItemInObjectCaseSensitive(obj, fd->name, bj);
                    snprintf(msg, sizeof(msg), "[L2] '%s': str→bool; ", fd->name);
                    log_append(log, log_sz, msg);
                    changes++;
                }
            }
        }
        /* Number 0/1 → Bool */
        else if (fd->type == CWIST_FIELD_BOOL && cJSON_IsNumber(item)) {
            cJSON *bj = (item->valuedouble != 0.0) ? cJSON_CreateTrue()
                                                   : cJSON_CreateFalse();
            if (bj) {
                cJSON_ReplaceItemInObjectCaseSensitive(obj, fd->name, bj);
                snprintf(msg, sizeof(msg), "[L2] '%s': num→bool; ", fd->name);
                log_append(log, log_sz, msg);
                changes++;
            }
        }
    }

    return changes;
}

/* ==========================================================================
 * Public API
 * ======================================================================== */

/**
 * @brief Recover malformed JSON using syntax repair, schema alignment, and an optional callback.
 *
 * The function first attempts a direct parse, then falls back to the internal
 * L1 syntax healer, and finally consults the user-provided SLLM callback when
 * configured. Each successful step records the recovery level, confidence, and
 * a short operator-facing log of the transformations that were applied.
 *
 * @param input Raw JSON text that may contain structural defects.
 * @param cfg Optional recovery configuration. NULL applies the built-in defaults.
 * @return Result structure describing the best recovery path that succeeded.
 */
cwist_heal_result_t cwist_json_heal(const char *input, const cwist_heal_config_t *cfg) {
    cwist_heal_result_t result;
    memset(&result, 0, sizeof(result));

    if (!input) return result;

    double threshold = (cfg && cfg->threshold > 0.0) ? cfg->threshold : 0.8;

    /* ---- Try direct parse ---- */
    cJSON *parsed = cJSON_Parse(input);
    if (parsed) {
        if (cfg && cfg->schema) {
            int changes = cwist_json_schema_align(parsed, cfg->schema,
                                                  result.log, CWIST_HEAL_LOG_MAX);
            if (changes > 0) {
                result.healed     = true;
                result.level      = 2;
                result.confidence = 1.0;
            }
        }
        result.json       = cJSON_PrintUnformatted(parsed);
        if (!result.healed) result.confidence = 1.0;
        cJSON_Delete(parsed);
        return result;
    }

    /* ---- L1: Syntax fix ---- */
    char *fixed = l1_heal(input, result.log, CWIST_HEAL_LOG_MAX);
    if (fixed) {
        parsed = cJSON_Parse(fixed);
        cwist_free(fixed);
        if (parsed) {
            result.level      = 1;
            result.healed     = true;
            result.confidence = 0.9;

            if (cfg && cfg->schema) {
                int ch = cwist_json_schema_align(parsed, cfg->schema,
                                                 result.log, CWIST_HEAL_LOG_MAX);
                if (ch > 0) result.level = 2;
            }

            if (result.confidence >= threshold)
                result.json = cJSON_PrintUnformatted(parsed);

            cJSON_Delete(parsed);
            return result;
        }
    }

    /* ---- L3: SLLM callback ---- */
    if (cfg && cfg->sllm_fn) {
        char *recovered = cfg->sllm_fn(input, cfg->schema, cfg->sllm_userdata);
        if (recovered) {
            parsed = cJSON_Parse(recovered);
            free(recovered); /* SLLM callback uses plain malloc per contract */
            if (parsed) {
                result.level      = 3;
                result.healed     = true;
                result.confidence = 0.7; /* heuristic */
                log_append(result.log, CWIST_HEAL_LOG_MAX, "[L3] SLLM recovery; ");

                if (result.confidence >= threshold)
                    result.json = cJSON_PrintUnformatted(parsed);

                cJSON_Delete(parsed);
            }
        }
    }

    return result;
}

/**
 * @brief Free heap storage owned by a healing result.
 * @param r Result object previously populated by cwist_json_heal().
 */
void cwist_heal_result_free(cwist_heal_result_t *r) {
    if (!r) return;
    /* result.json comes from cJSON_PrintUnformatted → plain malloc */
    if (r->json) {
        free(r->json);
        r->json = NULL;
    }
}
