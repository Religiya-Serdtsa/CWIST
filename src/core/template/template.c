#include "cwist/core/template/template.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/**
 * @file template.c
 * @brief Minimal template renderer supporting variable substitution and simple control flow.
 */

/** @brief Forward declaration for recursive block rendering. */
static cwist_sstring* render_internal(const char **template_str, const cJSON *context, int depth);

/** @brief Maximum nesting depth for includes/control structures. */
#define CWIST_TEMPLATE_MAX_DEPTH 32

/**
 * @brief Apply a simple filter to a string value in-place into a fixed buffer.
 * Supported filters: upper, lower.
 * @param value Input string.
 * @param filter Filter name.
 * @param out Output buffer.
 * @param out_len Output buffer size.
 */
static void apply_filter(const char *value, const char *filter, char *out, size_t out_len) {
    if (!value || !filter || out_len == 0) return;
    size_t i;
    for (i = 0; i < out_len - 1 && value[i]; i++) {
        if (strcmp(filter, "upper") == 0) {
            out[i] = (char)toupper((unsigned char)value[i]);
        } else if (strcmp(filter, "lower") == 0) {
            out[i] = (char)tolower((unsigned char)value[i]);
        } else {
            out[i] = value[i];
        }
    }
    out[i] = '\0';
}

/**
 * @brief Resolve a value from the current JSON context using dotted lookup syntax.
 * @param context Current object/array context used for template expansion.
 * @param key Dot-separated key path, or "." for the current loop item.
 * @return Matching cJSON node, or NULL when the path cannot be resolved.
 */
static const cJSON* get_value_from_context(const cJSON *context, const char *key) {
    if (!context || !key) return NULL;

    // Handle the special case of "." referring to the current context in a loop
    if (strcmp(key, ".") == 0) {
        return context;
    }

    char *key_copy = cwist_strdup(key);
    char *ptr_to_free = key_copy;
    char *token = strtok(key_copy, ".");
    const cJSON *current = context;

    while (token != NULL) {
        if (!cJSON_IsObject(current)) {
            current = NULL;
            break;
        }
        current = cJSON_GetObjectItem(current, token);
        if (!current) break;
        token = strtok(NULL, ".");
    }

    cwist_free(ptr_to_free);
    return current;
}


/**
 * @brief Recursively render a template string until the current control block ends.
 * @param template_str Cursor into the template source; advanced as tags are consumed.
 * @param context JSON context used for variable lookups and loop bindings.
 * @return Newly allocated rendered string fragment, or NULL on invalid input.
 */
static cwist_sstring* render_internal(const char **template_str, const cJSON *context, int depth) {
    if (!template_str || !*template_str || depth > CWIST_TEMPLATE_MAX_DEPTH) return NULL;

    cwist_sstring *output = cwist_sstring_create();
    if (!output) return NULL;
    const char *p = *template_str;
    const char *start = p;

    while (*p) {
        if (p[0] == '{' && (p[1] == '{' || p[1] == '%')) {
            /* Append text since last tag */
            cwist_sstring_append_len(output, start, p - start);

            if (p[1] == '{') { /* Variable: {{ key [| filter] }} */
                p += 2;
                const char *var_start = p;
                while (*p && (p[0] != '}' || p[1] != '}')) p++;

                char var_buf[256] = {0};
                size_t var_len = (size_t)(p - var_start);
                if (var_len >= sizeof(var_buf)) var_len = sizeof(var_buf) - 1;
                memcpy(var_buf, var_start, var_len);

                char *trimmed_var = var_buf;
                while (*trimmed_var == ' ') trimmed_var++;
                char *end = trimmed_var + strlen(trimmed_var) - 1;
                while (end > trimmed_var && *end == ' ') *end-- = '\0';

                char *filter = NULL;
                char *pipe = strstr(trimmed_var, "|");
                if (pipe) {
                    *pipe = '\0';
                    filter = pipe + 1;
                    while (*filter == ' ') filter++;
                    char *fe = filter + strlen(filter) - 1;
                    while (fe > filter && *fe == ' ') *fe-- = '\0';
                }

                const cJSON *value = get_value_from_context(context, trimmed_var);
                if (value) {
                    char num_str[64] = {0};
                    const char *raw = NULL;
                    if (cJSON_IsString(value)) {
                        raw = value->valuestring;
                    } else if (cJSON_IsNumber(value)) {
                        snprintf(num_str, sizeof(num_str), "%g", value->valuedouble);
                        raw = num_str;
                    } else if (cJSON_IsTrue(value)) {
                        raw = "true";
                    } else if (cJSON_IsFalse(value)) {
                        raw = "false";
                    }
                    if (raw) {
                        if (filter) {
                            char filtered[512] = {0};
                            apply_filter(raw, filter, filtered, sizeof(filtered));
                            cwist_sstring_append(output, filtered);
                        } else {
                            cwist_sstring_append(output, raw);
                        }
                    }
                }
                p += 2;
                start = p;

            } else if (p[1] == '%') { /* Tag: {% ... %} */
                p += 2;
                const char *tag_start = p;
                while (*p && (p[0] != '%' || p[1] != '}')) p++;

                char tag[256] = {0};
                size_t tag_len = (size_t)(p - tag_start);
                if (tag_len >= sizeof(tag)) tag_len = sizeof(tag) - 1;
                memcpy(tag, tag_start, tag_len);

                char *cmd = strtok(tag, " \t\n");
                if (!cmd) {
                    p += 2;
                    start = p;
                    continue;
                }

                if (strcmp(cmd, "if") == 0) {
                    char *tok = strtok(NULL, " \t\n");
                    bool negate = false;
                    if (tok && strcmp(tok, "not") == 0) {
                        negate = true;
                        tok = strtok(NULL, " \t\n");
                    }
                    const cJSON *val = tok ? get_value_from_context(context, tok) : NULL;
                    bool cond = false;
                    if (val) {
                        cond = cJSON_IsTrue(val) ||
                               (cJSON_IsString(val) && strlen(val->valuestring) > 0) ||
                               (cJSON_IsObject(val) && cJSON_GetArraySize(val) > 0) ||
                               (cJSON_IsArray(val) && cJSON_GetArraySize(val) > 0) ||
                               (cJSON_IsNumber(val) && val->valuedouble != 0);
                    }
                    if (negate) cond = !cond;

                    const char *block_start = p + 2;
                    const char *else_pos = NULL;
                    const char *block_end = NULL;
                    const char *scan = block_start;
                    int nested = 0;
                    while (*scan) {
                        if (scan[0] == '{' && scan[1] == '%') {
                            const char *inner = scan + 2;
                            while (*inner == ' ' || *inner == '\t' || *inner == '\n') inner++;
                            if (strncmp(inner, "if ", 3) == 0 || strncmp(inner, "if\t", 3) == 0 || strncmp(inner, "if\n", 3) == 0) {
                                nested++;
                            } else if (strncmp(inner, "endif", 5) == 0) {
                                if (nested == 0) {
                                    block_end = scan;
                                    break;
                                }
                                nested--;
                            } else if (!else_pos && nested == 0 && strncmp(inner, "else", 4) == 0 &&
                                       (inner[4] == ' ' || inner[4] == '\t' || inner[4] == '\n' || inner[4] == '%')) {
                                else_pos = scan;
                            }
                        }
                        scan++;
                    }

                    if (cond) {
                        const char *block_p = block_start;
                        cwist_sstring *rendered_block = render_internal(&block_p, context, depth + 1);
                        if (rendered_block) {
                            cwist_sstring_append(output, rendered_block->data);
                            cwist_sstring_destroy(rendered_block);
                        }
                    } else if (else_pos) {
                        const char *else_close = strstr(else_pos, "%}");
                        const char *else_p = else_close ? else_close + 2 : else_pos + strlen("{% else %}");
                        cwist_sstring *rendered_block = render_internal(&else_p, context, depth + 1);
                        if (rendered_block) {
                            cwist_sstring_append(output, rendered_block->data);
                            cwist_sstring_destroy(rendered_block);
                        }
                    }

                    if (block_end) {
                        const char *endif_close = strstr(block_end, "%}");
                        p = endif_close ? endif_close + 2 : block_end + strlen("{% endif %}");
                    } else {
                        p += 2;
                    }
                    start = p;

                } else if (strcmp(cmd, "for") == 0) {
                    char *item_name = strtok(NULL, " \t\n");
                    strtok(NULL, " \t\n"); /* "in" */
                    char *array_name = strtok(NULL, " \t\n");

                    const cJSON *array = get_value_from_context(context, array_name);

                    const char *block_start = p + 2;
                    const char *block_end = strstr(block_start, "{% endfor %}");

                    if (cJSON_IsArray(array)) {
                        cJSON *item;
                        cJSON_ArrayForEach(item, array) {
                            cJSON *loop_context = cJSON_Duplicate(context, 1);
                            if (!loop_context) continue;
                            cJSON_AddItemToObject(loop_context, item_name, cJSON_Duplicate(item, 1));

                            const char *loop_p = block_start;
                            cwist_sstring *rendered_block = render_internal(&loop_p, loop_context, depth + 1);
                            if (rendered_block) {
                                cwist_sstring_append(output, rendered_block->data);
                                cwist_sstring_destroy(rendered_block);
                            }
                            cJSON_Delete(loop_context);
                        }
                    }
                    if (block_end) {
                        const char *endfor_close = strstr(block_end, "%}");
                        p = endfor_close ? endfor_close + 2 : block_end + strlen("{% endfor %}");
                    } else {
                        p += 2;
                    }
                    start = p;

                } else if (strcmp(cmd, "include") == 0) {
                    char *inc_cmd = strstr(tag, "include");
                    char *path = inc_cmd ? inc_cmd + strlen("include") : NULL;
                    if (path) {
                        while (*path == ' ' || *path == '\t' || *path == '\n') path++;
                        char *end = path + strlen(path) - 1;
                        while (end > path && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '"')) *end-- = '\0';
                        if (path[0] == '"') path++;
                        if (path[0]) {
                            cwist_sstring *included = cwist_template_render_file(path, context);
                            if (included) {
                                cwist_sstring_append(output, included->data);
                                cwist_sstring_destroy(included);
                            }
                        }
                    }
                    p += 2;
                    start = p;

                } else if (strcmp(cmd, "endif") == 0 || strcmp(cmd, "endfor") == 0 || strcmp(cmd, "else") == 0) {
                    *template_str = p + 2;
                    return output;
                } else {
                     p += 2;
                     start = p;
                }
            }
        } else {
            p++;
        }
    }

    cwist_sstring_append_len(output, start, p - start);
    *template_str = p;
    return output;
}

/**
 * @brief Render an in-memory template string against a JSON context object.
 * @param template_str Template source to render.
 * @param context JSON object supplying values for substitutions and control flow.
 * @return Heap-allocated rendered output, or NULL on failure.
 */
cwist_sstring* cwist_template_render(const char *template_str, const cJSON *context) {
    const char *p = template_str;
    return render_internal(&p, context, 0);
}

/**
 * @brief Load a template file from disk and render it against a JSON context.
 * @param file_path Path to the template file to open.
 * @param context JSON object supplying values for substitutions and control flow.
 * @return Heap-allocated rendered output, or NULL when file IO or rendering fails.
 */
cwist_sstring* cwist_template_render_file(const char *file_path, const cJSON *context) {
    FILE *f = fopen(file_path, "rb");
    if (!f) {
        perror("Failed to open template file");
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *template_str = cwist_alloc(len + 1);
    if (!template_str) {
        fclose(f);
        return NULL;
    }

    fread(template_str, 1, len, f);
    template_str[len] = '\0';
    fclose(f);

    cwist_sstring *result = cwist_template_render(template_str, context);
    cwist_free(template_str);

    return result;
}
