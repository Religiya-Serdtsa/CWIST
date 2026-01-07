#include "cwist/core/template/template.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Forward declaration
static cwist_sstring* render_internal(const char **template_str, const cJSON *context);

// Helper to get a value from cJSON context, supports dot notation for nested objects.
static const cJSON* get_value_from_context(const cJSON *context, const char *key) {
    if (!context || !key) return NULL;

    // Handle the special case of "." referring to the current context in a loop
    if (strcmp(key, ".") == 0) {
        return context;
    }

    char *key_copy = strdup(key);
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

    free(ptr_to_free);
    return current;
}


static cwist_sstring* render_internal(const char **template_str, const cJSON *context) {
    if (!template_str || !*template_str) return NULL;

    cwist_sstring *output = cwist_sstring_create();
    const char *p = *template_str;
    const char *start = p;

    while (*p) {
        if (p[0] == '{' && (p[1] == '{' || p[1] == '%')) {
            // Append text since last tag
            cwist_sstring_append_len(output, start, p - start);

            if (p[1] == '{') { // Variable: {{ key }}
                p += 2;
                const char *var_start = p;
                while (*p && (p[0] != '}' || p[1] != '}')) p++;
                
                char var_name[256] = {0};
                strncpy(var_name, var_start, p - var_start);
                char *trimmed_var = var_name;
                while (*trimmed_var == ' ') trimmed_var++;
                char *end = trimmed_var + strlen(trimmed_var) - 1;
                while (end > trimmed_var && *end == ' ') *end-- = '\0';

                const cJSON *value = get_value_from_context(context, trimmed_var);
                if (value) {
                    if (cJSON_IsString(value)) {
                        cwist_sstring_append(output, value->valuestring);
                    } else if (cJSON_IsNumber(value)) {
                        char num_str[64];
                        snprintf(num_str, sizeof(num_str), "%g", value->valuedouble);
                        cwist_sstring_append(output, num_str);
                    } else if (cJSON_IsTrue(value)) {
                        cwist_sstring_append(output, "true");
                    } else if (cJSON_IsFalse(value)) {
                        cwist_sstring_append(output, "false");
                    }
                }
                p += 2;
                start = p;

            } else if (p[1] == '%') { // Tag: {% ... %}
                p += 2;
                const char *tag_start = p;
                while (*p && (p[0] != '%' || p[1] != '}')) p++;
                
                char tag[256] = {0};
                strncpy(tag, tag_start, p - tag_start);

                char *cmd = strtok(tag, " \t\n");
                
                if (strcmp(cmd, "if") == 0) {
                    char *key = strtok(NULL, " \t\n");
                    const cJSON *val = get_value_from_context(context, key);
                    
                    const char* block_start = p + 2;
                    const char* block_end = strstr(block_start, "{% endif %}");
                    
                    if (val && (cJSON_IsTrue(val) || (cJSON_IsString(val) && strlen(val->valuestring) > 0) || (cJSON_IsObject(val) && cJSON_GetArraySize(val) > 0))) {
                        cwist_sstring *rendered_block = render_internal(&block_start, context);
                        cwist_sstring_append(output, rendered_block->data);
                        cwist_sstring_destroy(rendered_block);
                    }
                    
                    p = block_end ? block_end + strlen("{% endif %}") : p + 2;
                    start = p;

                } else if (strcmp(cmd, "for") == 0) {
                    char *item_name = strtok(NULL, " \t\n");
                    strtok(NULL, " \t\n"); // "in"
                    char *array_name = strtok(NULL, " \t\n");

                    const cJSON *array = get_value_from_context(context, array_name);
                    
                    const char* block_start = p + 2;
                    const char* block_end = strstr(block_start, "{% endfor %}");

                    if (cJSON_IsArray(array)) {
                        cJSON *item;
                        cJSON_ArrayForEach(item, array) {
                            cJSON *loop_context = cJSON_Duplicate(context, 1);
                            cJSON_AddItemToObject(loop_context, item_name, cJSON_Duplicate(item, 1));
                            
                            const char *loop_p = block_start;
                            cwist_sstring *rendered_block = render_internal(&loop_p, loop_context);
                            cwist_sstring_append(output, rendered_block->data);

                            cwist_sstring_destroy(rendered_block);
                            cJSON_Delete(loop_context);
                        }
                    }
                    p = block_end ? block_end + strlen("{% endfor %}") : p + 2;
                    start = p;

                } else if (strcmp(cmd, "endif") == 0 || strcmp(cmd, "endfor") == 0) {
                    *template_str = p + strlen(cmd) + 4;
                    cwist_sstring_append_len(output, start, p - (start + 2) - strlen(cmd));
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

cwist_sstring* cwist_template_render(const char *template_str, const cJSON *context) {
    const char *p = template_str;
    return render_internal(&p, context);
}

cwist_sstring* cwist_template_render_file(const char *file_path, const cJSON *context) {
    FILE *f = fopen(file_path, "rb");
    if (!f) {
        perror("Failed to open template file");
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *template_str = malloc(len + 1);
    if (!template_str) {
        fclose(f);
        return NULL;
    }

    fread(template_str, 1, len, f);
    template_str[len] = '\0';
    fclose(f);

    cwist_sstring *result = cwist_template_render(template_str, context);
    free(template_str);

    return result;
}