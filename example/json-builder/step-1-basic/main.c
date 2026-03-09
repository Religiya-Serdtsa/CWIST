#include <stdio.h>
#include <cwist/core/utils/json_builder.h>

int main() {
    printf("=== JSON Builder Tutorial ===\n");

    /* ---- 1. Simple flat object ---- */
    printf("\n[Flat object]\n");
    cwist_json_builder *jb = cwist_json_builder_create();
    cwist_json_begin_object(jb);
    cwist_json_add_string(jb, "name",    "Alice");
    cwist_json_add_int   (jb, "age",     30);
    cwist_json_add_bool  (jb, "active",  true);
    cwist_json_add_null  (jb, "note");
    cwist_json_end_object(jb);
    printf("%s\n", cwist_json_get_raw(jb));
    cwist_json_builder_destroy(jb);

    /* ---- 2. Nested object ---- */
    printf("\n[Nested object]\n");
    jb = cwist_json_builder_create();
    cwist_json_begin_object(jb);
    cwist_json_add_string(jb, "status", "ok");
    cwist_json_add_int   (jb, "code",   200);
    /* Manually embed a nested object as a raw string field */
    cwist_sstring_append(jb->buffer, ",\"data\":{\"id\":1,\"value\":42}");
    cwist_json_end_object(jb);
    printf("%s\n", cwist_json_get_raw(jb));
    cwist_json_builder_destroy(jb);

    /* ---- 3. Array of objects ---- */
    printf("\n[Array of objects]\n");
    jb = cwist_json_builder_create();
    cwist_json_begin_array(jb, NULL);
    for (int i = 1; i <= 3; i++) {
        if (i > 1) cwist_sstring_append(jb->buffer, ",");
        cwist_sstring_append(jb->buffer, "{");
        jb->needs_comma = false;
        cwist_json_add_int(jb, "id", i);
        char label[32];
        snprintf(label, sizeof(label), "item-%d", i);
        cwist_json_add_string(jb, "label", label);
        cwist_sstring_append(jb->buffer, "}");
    }
    cwist_json_end_array(jb);
    printf("%s\n", cwist_json_get_raw(jb));
    cwist_json_builder_destroy(jb);

    printf("\n=== Done ===\n");
    return 0;
}
