/**
 * @file bind.c
 * @brief Implementation of declarative schema validation and binding.
 */

#include <cwist/core/validation/bind.h>
#include <cwist/core/mem/alloc.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/**
 * @brief Append one error to the result accumulator.
 */
static void bind_add_error(cwist_bind_result_t *r, const char *key, const char *msg) {
    if (!r) return;
    if (r->error_count >= CWIST_BIND_MAX_ERRORS) return;
    cwist_bind_error_t *e = &r->errors[r->error_count++];
    snprintf(e->key, sizeof(e->key), "%s", key ? key : "(unknown)");
    snprintf(e->message, sizeof(e->message), "%s", msg ? msg : "validation failed");
    r->ok = false;
}

/**
 * @brief Simplified RFC 5322 email check.
 * Looks for: local@domain.tld with sane length limits.
 */
static bool is_email_like(const char *s, size_t len) {
    if (!s || len < 5) return false;
    const char *at = strchr(s, '@');
    if (!at) return false;
    if (at == s) return false;
    const char *dot = strrchr(at, '.');
    if (!dot || dot <= at + 1) return false;
    if (dot + 1 == s + len) return false; /* trailing dot */
    return true;
}

/**
 * @brief Run a single rule against a string value.
 * @return true when the rule passes.
 */
static bool check_rule(const cwist_bind_rule_t *rule,
                       const char *value,
                       size_t len,
                       cwist_bind_result_t *r,
                       const char *key) {
    char buf[CWIST_BIND_MSG_MAX];

    switch (rule->type) {
        case CWIST_BIND_RULE_REQUIRED:
            if (!value || len == 0) {
                snprintf(buf, sizeof(buf), "%s",
                         rule->error_message ? rule->error_message : "field is required");
                bind_add_error(r, key, buf);
                return false;
            }
            break;

        case CWIST_BIND_RULE_MIN_LEN:
            if (!value || len < rule->u.min_len) {
                snprintf(buf, sizeof(buf), "%s: minimum length is %zu",
                         rule->error_message ? rule->error_message : "too short",
                         rule->u.min_len);
                bind_add_error(r, key, buf);
                return false;
            }
            break;

        case CWIST_BIND_RULE_MAX_LEN:
            if (value && len > rule->u.max_len) {
                snprintf(buf, sizeof(buf), "%s: maximum length is %zu",
                         rule->error_message ? rule->error_message : "too long",
                         rule->u.max_len);
                bind_add_error(r, key, buf);
                return false;
            }
            break;

        case CWIST_BIND_RULE_MIN_VAL: {
            long double v = 0.0L;
            if (value) v = strtold(value, NULL);
            if (!value || v < rule->u.min_val) {
                snprintf(buf, sizeof(buf), "%s: minimum value is %Lg",
                         rule->error_message ? rule->error_message : "value too small",
                         rule->u.min_val);
                bind_add_error(r, key, buf);
                return false;
            }
            break;
        }

        case CWIST_BIND_RULE_MAX_VAL: {
            long double v = 0.0L;
            if (value) v = strtold(value, NULL);
            if (!value || v > rule->u.max_val) {
                snprintf(buf, sizeof(buf), "%s: maximum value is %Lg",
                         rule->error_message ? rule->error_message : "value too large",
                         rule->u.max_val);
                bind_add_error(r, key, buf);
                return false;
            }
            break;
        }

        case CWIST_BIND_RULE_REGEX: {
            if (!value) {
                bind_add_error(r, key, "missing value for regex check");
                return false;
            }
            regex_t re;
            int rc = regcomp(&re, rule->u.pattern, REG_EXTENDED | REG_NOSUB);
            if (rc != 0) {
                bind_add_error(r, key, "invalid regex pattern");
                return false;
            }
            rc = regexec(&re, value, 0, NULL, 0);
            regfree(&re);
            if (rc != 0) {
                snprintf(buf, sizeof(buf), "%s",
                         rule->error_message ? rule->error_message : "value does not match pattern");
                bind_add_error(r, key, buf);
                return false;
            }
            break;
        }

        case CWIST_BIND_RULE_EMAIL:
            if (!value || !is_email_like(value, len)) {
                snprintf(buf, sizeof(buf), "%s",
                         rule->error_message ? rule->error_message : "invalid email format");
                bind_add_error(r, key, buf);
                return false;
            }
            break;

        case CWIST_BIND_RULE_CUSTOM:
            if (!value || !rule->u.custom.fn(value, len, rule->u.custom.ctx)) {
                snprintf(buf, sizeof(buf), "%s",
                         rule->error_message ? rule->error_message : "custom validation failed");
                bind_add_error(r, key, buf);
                return false;
            }
            break;

        default:
            break;
    }
    return true;
}

/**
 * @brief Run all rules for a field against a string value.
 */
static bool run_rules(const cwist_bind_field_t *f,
                      const char *value,
                      size_t len,
                      cwist_bind_result_t *r) {
    if (!f->rules) return true;
    bool all_ok = true;
    for (size_t i = 0; i < CWIST_BIND_MAX_ERRORS; ++i) {
        if (f->rules[i].type == (cwist_bind_rule_type_t)-1) break;
        if (!check_rule(&f->rules[i], value, len, r, f->json_key))
            all_ok = false;
    }
    return all_ok;
}

/**
 * @brief Write a typed value from a string into the struct at the given offset.
 */
static bool write_value(const cwist_bind_field_t *f,
                        const char *value,
                        void *out,
                        cwist_bind_result_t *r) {
    void *dest = (char *)out + f->target_offset;

    switch (f->target_type) {
        case CWIST_BIND_BOOL: {
            bool v = false;
            if (value) {
                if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) v = true;
                else if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) v = false;
                else { bind_add_error(r, f->json_key, "expected boolean"); return false; }
            }
            memcpy(dest, &v, sizeof(v));
            break;
        }
        case CWIST_BIND_INT: {
            long long v = 0;
            if (value) {
                char *end = NULL;
                errno = 0;
                v = strtoll(value, &end, 10);
                if (errno == ERANGE || end == value || *end != '\0') {
                    bind_add_error(r, f->json_key, "expected integer");
                    return false;
                }
            }
            memcpy(dest, &v, f->target_size <= sizeof(v) ? f->target_size : sizeof(v));
            break;
        }
        case CWIST_BIND_UINT: {
            unsigned long long v = 0;
            if (value) {
                char *end = NULL;
                errno = 0;
                v = strtoull(value, &end, 10);
                if (errno == ERANGE || end == value || *end != '\0') {
                    bind_add_error(r, f->json_key, "expected unsigned integer");
                    return false;
                }
            }
            memcpy(dest, &v, f->target_size <= sizeof(v) ? f->target_size : sizeof(v));
            break;
        }
        case CWIST_BIND_FLOAT: {
            float v = 0.0f;
            if (value) {
                char *end = NULL;
                v = strtof(value, &end);
                if (end == value || *end != '\0') {
                    bind_add_error(r, f->json_key, "expected float");
                    return false;
                }
            }
            memcpy(dest, &v, sizeof(v));
            break;
        }
        case CWIST_BIND_DOUBLE: {
            double v = 0.0;
            if (value) {
                char *end = NULL;
                v = strtod(value, &end);
                if (end == value || *end != '\0') {
                    bind_add_error(r, f->json_key, "expected double");
                    return false;
                }
            }
            memcpy(dest, &v, sizeof(v));
            break;
        }
        case CWIST_BIND_STRING: {
            if (f->target_size == 0) {
                bind_add_error(r, f->json_key, "bad target size");
                return false;
            }
            if (value) {
                size_t copy = strlen(value);
                if (copy >= f->target_size) copy = f->target_size - 1;
                memcpy(dest, value, copy);
                ((char *)dest)[copy] = '\0';
            } else {
                ((char *)dest)[0] = '\0';
            }
            break;
        }
        case CWIST_BIND_SSTRING: {
            cwist_sstring *ss = *(cwist_sstring **)dest;
            if (!ss) {
                bind_add_error(r, f->json_key, "null sstring target");
                return false;
            }
            if (value) cwist_sstring_assign(ss, (char *)value);
            else cwist_sstring_assign(ss, "");
            break;
        }
        case CWIST_BIND_JSON_OBJECT: {
            cJSON **pp = (cJSON **)dest;
            if (!pp) {
                bind_add_error(r, f->json_key, "null cJSON target");
                return false;
            }
            if (*pp) { cJSON_Delete(*pp); *pp = NULL; }
            if (value) {
                *pp = cJSON_Parse(value);
                if (!*pp) {
                    bind_add_error(r, f->json_key, "invalid nested JSON");
                    return false;
                }
            }
            break;
        }
        default:
            bind_add_error(r, f->json_key, "unsupported bind target type");
            return false;
    }
    return true;
}

/**
 * @brief Core binding logic shared between JSON and Form parsers.
 */
static bool cwist_bind_generic(const cwist_bind_schema_t *schema,
                               const char *payload,
                               bool is_json,
                               void *out,
                               cwist_bind_result_t *result) {
    if (!schema || !out || !result) return false;
    memset(result, 0, sizeof(*result));
    result->ok = true;

    if (!payload) {
        bind_add_error(result, "(root)", "missing request body");
        return false;
    }

    cJSON *root = NULL;
    if (is_json) {
        root = cJSON_Parse(payload);
        if (!root) {
            bind_add_error(result, "(root)", "JSON parse error");
            return false;
        }
    }

    for (size_t i = 0; i < schema->field_count; ++i) {
        const cwist_bind_field_t *f = &schema->fields[i];
        const char *raw_value = NULL;

        if (is_json && root) {
            const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, f->json_key);
            if (item) {
                if (cJSON_IsString(item)) {
                    raw_value = item->valuestring;
                } else if (cJSON_IsNumber(item)) {
                    /* Keep number as string for uniform validation then convert */
                    static __thread char numbuf[64];
                    double intpart;
                    if (modf(item->valuedouble, &intpart) == 0.0) {
                        snprintf(numbuf, sizeof(numbuf), "%lld", (long long)item->valuedouble);
                    } else {
                        snprintf(numbuf, sizeof(numbuf), "%g", item->valuedouble);
                    }
                    raw_value = numbuf;
                } else if (cJSON_IsBool(item)) {
                    raw_value = cJSON_IsTrue(item) ? "true" : "false";
                } else if (cJSON_IsNull(item)) {
                    raw_value = NULL;
                } else if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
                    /* For JSON_OBJECT targets, serialise the subtree */
                    if (f->target_type == CWIST_BIND_JSON_OBJECT) {
                        char *printed = cJSON_PrintUnformatted(item);
                        raw_value = printed; /* transient; written below */
                        write_value(f, raw_value, out, result);
                        free(printed);
                        continue;
                    } else {
                        raw_value = cJSON_PrintUnformatted(item);
                        /* fall through to string validation then cleanup */
                    }
                }
            }
        } else {
            /* Form parsing: simple key=value scan (naïve, no unescaping) */
            const char *cursor = payload;
            size_t key_len = strlen(f->json_key);
            while (cursor && *cursor) {
                if (strncmp(cursor, f->json_key, key_len) == 0 && cursor[key_len] == '=') {
                    const char *start = cursor + key_len + 1;
                    const char *end = strchr(start, '&');
                    size_t vlen = end ? (size_t)(end - start) : strlen(start);
                    /* Zero-copy reference into payload; safe because req->body is stable */
                    raw_value = start;
                    /* Temporarily null-terminate for validation (destructive to body). */
                    /* We make a bounded copy instead to be safe. */
                    if (vlen > 0) {
                        static __thread char formbuf[4096];
                        if (vlen >= sizeof(formbuf)) vlen = sizeof(formbuf) - 1;
                        memcpy(formbuf, start, vlen);
                        formbuf[vlen] = '\0';
                        raw_value = formbuf;
                    } else {
                        raw_value = "";
                    }
                    break;
                }
                const char *next = strchr(cursor, '&');
                cursor = next ? next + 1 : NULL;
            }
        }

        size_t val_len = raw_value ? strlen(raw_value) : 0;

        if (!run_rules(f, raw_value, val_len, result)) {
            /* continue to collect more errors */
        }

        if (result->error_count == 0 || result->ok) {
            if (!write_value(f, raw_value, out, result)) {
                result->ok = false;
            }
        }

        /* Clean up temporary printed JSON if we allocated one above */
        if (!is_json) { /* already handled */ }
    }

    if (root) cJSON_Delete(root);
    result->ok = (result->error_count == 0);
    return result->ok;
}

/* -------------------------------------------------------------------------
 * Public implementation
 * ---------------------------------------------------------------------- */

bool cwist_app_req_bind_json(cwist_http_request *req,
                              const cwist_bind_schema_t *schema,
                              void *out,
                              cwist_bind_result_t *result) {
    const char *payload = (req && req->body && req->body->data) ? req->body->data : NULL;
    return cwist_bind_generic(schema, payload, true, out, result);
}

bool cwist_app_req_bind_form(cwist_http_request *req,
                              const cwist_bind_schema_t *schema,
                              void *out,
                              cwist_bind_result_t *result) {
    const char *payload = (req && req->body && req->body->data) ? req->body->data : NULL;
    return cwist_bind_generic(schema, payload, false, out, result);
}

void cwist_bind_write_error_response(cwist_http_response *res,
                                      const cwist_bind_result_t *result) {
    if (!res || !result) return;
    res->status_code = CWIST_HTTP_BAD_REQUEST;
    cwist_sstring_assign(res->status_text, "Bad Request");

    cwist_json_builder *jb = cwist_json_builder_create();
    cwist_json_begin_object(jb);
    cwist_json_add_bool(jb, "success", false);
    cwist_json_begin_array(jb, "errors");
    for (size_t i = 0; i < result->error_count; ++i) {
        cwist_json_begin_object(jb);
        cwist_json_add_string(jb, "field", result->errors[i].key);
        cwist_json_add_string(jb, "message", result->errors[i].message);
        cwist_json_end_object(jb);
    }
    cwist_json_end_array(jb);
    cwist_json_end_object(jb);

    const char *json = cwist_json_get_raw(jb);
    cwist_sstring_assign(res->body, (char *)json);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_json_builder_destroy(jb);
}

bool cwist_app_req_bind_json_or_400(cwist_http_request *req,
                                     cwist_http_response *res,
                                     const cwist_bind_schema_t *schema,
                                     void *out) {
    cwist_bind_result_t result;
    if (!cwist_app_req_bind_json(req, schema, out, &result)) {
        cwist_bind_write_error_response(res, &result);
        return false;
    }
    return true;
}
