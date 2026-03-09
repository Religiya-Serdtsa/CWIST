#include <stdio.h>
#include <cwist/core/template/template.h>
#include <cjson/cJSON.h>

int main() {
    printf("=== Template Rendering Tutorial ===\n");

    /* ---- 1. Variable substitution ---- */
    printf("\n[Variable substitution]\n");
    cJSON *ctx = cJSON_CreateObject();
    cJSON_AddStringToObject(ctx, "name", "CWIST");
    cJSON_AddNumberToObject(ctx, "version", 1);

    const char *tmpl1 = "Hello, {{ name }}! This is version {{ version }}.";
    cwist_sstring *out = cwist_template_render(tmpl1, ctx);
    if (out) {
        printf("Output: %s\n", out->data);
        cwist_sstring_destroy(out);
    }
    cJSON_Delete(ctx);

    /* ---- 2. Conditional block ---- */
    printf("\n[Conditional block]\n");
    ctx = cJSON_CreateObject();
    cJSON_AddTrueToObject(ctx,  "logged_in");
    cJSON_AddStringToObject(ctx, "user", "Alice");

    const char *tmpl2 =
        "{% if logged_in %}Welcome back, {{ user }}!{% endif %}"
        "{% if guest %}Please log in.{% endif %}";
    out = cwist_template_render(tmpl2, ctx);
    if (out) {
        printf("Output: %s\n", out->data);
        cwist_sstring_destroy(out);
    }
    cJSON_Delete(ctx);

    /* ---- 3. Loop over array ---- */
    printf("\n[Loop over array]\n");
    ctx = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(ctx, "fruits");
    cJSON *f1 = cJSON_CreateObject(); cJSON_AddStringToObject(f1, "name", "Apple");
    cJSON *f2 = cJSON_CreateObject(); cJSON_AddStringToObject(f2, "name", "Banana");
    cJSON *f3 = cJSON_CreateObject(); cJSON_AddStringToObject(f3, "name", "Cherry");
    cJSON_AddItemToArray(items, f1);
    cJSON_AddItemToArray(items, f2);
    cJSON_AddItemToArray(items, f3);

    const char *tmpl3 =
        "Fruit list:\n"
        "{% for fruit in fruits %}"
        "  - {{ fruit.name }}\n"
        "{% endfor %}";
    out = cwist_template_render(tmpl3, ctx);
    if (out) {
        printf("Output:\n%s\n", out->data);
        cwist_sstring_destroy(out);
    }
    cJSON_Delete(ctx);

    printf("=== Done ===\n");
    return 0;
}
