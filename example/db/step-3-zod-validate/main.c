#include <stdio.h>
#include <cwist/core/db/sql.h>
#include <cwist/core/utils/zod.h>
#include <cwist/core/utils/json_heal.h>
#include <cjson/cJSON.h>

/* Define a strict schema for a "product" object */
static const cwist_schema_field_t product_fields[] = {
    { "id",    {NULL},              CWIST_FIELD_INT,    true  },
    { "name",  {NULL},              CWIST_FIELD_STRING, true  },
    { "price", {"cost", "amount"},  CWIST_FIELD_FLOAT,  true  },
    { "stock", {"qty", "quantity"}, CWIST_FIELD_INT,    false },
};
static const cwist_schema_t product_schema = { product_fields, 4 };

static void validate(const char *label, const char *json_str) {
    printf("\n[%s]\n  input: %s\n", label, json_str);

    cJSON *parsed = NULL;
    cwist_zod_result_t r = cwist_zod_parse(json_str, &product_schema, &parsed);

    if (r.valid) {
        printf("  result: VALID\n");
        cJSON_Delete(parsed);
    } else {
        printf("  result: INVALID (%d error%s)\n",
               r.error_count, r.error_count == 1 ? "" : "s");
        for (int i = 0; i < r.error_count; i++) {
            printf("    [%s] %s\n",
                   r.errors[i].field, r.errors[i].message);
        }
    }
}

int main() {
    printf("=== Zod-style Schema Validation ===\n");

    /* 1. Fully valid object */
    validate("valid",
        "{\"id\":1,\"name\":\"Widget\",\"price\":9.99,\"stock\":100}");

    /* 2. Missing required field 'price' */
    validate("missing required field",
        "{\"id\":2,\"name\":\"Gadget\"}");

    /* 3. Wrong type for 'id' */
    validate("wrong type for id",
        "{\"id\":\"three\",\"name\":\"Doohickey\",\"price\":4.5}");

    /* 4. Alias resolution: 'cost' should be accepted as 'price' via heal first */
    printf("\n[alias via cwist_json_heal + cwist_zod_validate]\n");
    const char *aliased = "{\"id\":4,\"name\":\"Thingamajig\",\"cost\":12.0}";
    cwist_heal_config_t cfg = { .threshold = 0.5, .schema = &product_schema };
    cwist_heal_result_t hr  = cwist_json_heal(aliased, &cfg);
    printf("  healed: %s\n", hr.json ? hr.json : "(null)");
    if (hr.json) {
        cJSON *healed_obj = cJSON_Parse(hr.json);
        if (healed_obj) {
            cwist_zod_result_t r2 = cwist_zod_validate(healed_obj, &product_schema);
            printf("  after heal: %s\n", r2.valid ? "VALID" : "INVALID");
            cJSON_Delete(healed_obj);
        }
    }
    cwist_heal_result_free(&hr);

    printf("\n=== Done ===\n");
    return 0;
}
