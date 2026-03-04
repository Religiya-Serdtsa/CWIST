/**
 * @file test_json_heal.c
 * @brief Unit tests for cwist_json_heal (L1/L2/L3), cwist_zod_validate,
 *        cwist_db_insert_healed, and cwist_db_query_strict.
 */

#include <cwist/core/utils/json_heal.h>
#include <cwist/core/utils/zod.h>
#include <cwist/core/db/sql.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

/* --------------------------------------------------------------------------
 * Shared schema used by most tests
 * ------------------------------------------------------------------------ */

static const cwist_schema_field_t s_fields[] = {
    { "user_id", {"userId",  "uid",  NULL}, CWIST_FIELD_INT,    true  },
    { "name",    {NULL},                    CWIST_FIELD_STRING, true  },
    { "active",  {"is_active", NULL},       CWIST_FIELD_BOOL,   false },
    { "score",   {"Score",    NULL},        CWIST_FIELD_FLOAT,  false },
};
static const cwist_schema_t s_schema = { s_fields, 4 };

/* ==========================================================================
 * L1 Syntax healing tests
 * ======================================================================== */

static void test_l1_already_valid(void) {
    printf("L1: already-valid JSON passes through unchanged...\n");
    const char *input = "{\"user_id\":1,\"name\":\"Alice\"}";
    cwist_heal_result_t r = cwist_json_heal(input, NULL);
    assert(r.json != NULL);
    assert(r.healed == false);
    assert(r.level  == 0);
    assert(r.confidence == 1.0);

    /* Verify it round-trips through cJSON */
    cJSON *parsed = cJSON_Parse(r.json);
    assert(parsed != NULL);
    cJSON_Delete(parsed);

    cwist_heal_result_free(&r);
    printf("  Passed.\n");
}

static void test_l1_trailing_comma(void) {
    printf("L1: trailing comma before }...\n");
    const char *input = "{\"a\":1,\"b\":2,}";
    cwist_heal_result_t r = cwist_json_heal(input, NULL);
    assert(r.json   != NULL);
    assert(r.healed == true);
    assert(r.level  == 1);

    cJSON *parsed = cJSON_Parse(r.json);
    assert(parsed != NULL);
    cJSON *a = cJSON_GetObjectItem(parsed, "a");
    assert(a && a->valueint == 1);
    cJSON_Delete(parsed);

    cwist_heal_result_free(&r);
    printf("  Passed.\n");
}

static void test_l1_missing_closers(void) {
    printf("L1: missing closing brace and bracket...\n");
    const char *input = "{\"items\":[1,2,3";
    cwist_heal_result_t r = cwist_json_heal(input, NULL);
    assert(r.json   != NULL);
    assert(r.healed == true);
    assert(r.level  == 1);

    cJSON *parsed = cJSON_Parse(r.json);
    assert(parsed != NULL);
    cJSON *items = cJSON_GetObjectItem(parsed, "items");
    assert(items && cJSON_IsArray(items));
    assert(cJSON_GetArraySize(items) == 3);
    cJSON_Delete(parsed);

    cwist_heal_result_free(&r);
    printf("  Passed.\n");
}

static void test_l1_line_comment(void) {
    printf("L1: JavaScript-style // comment stripped...\n");
    const char *input = "{\"x\":42 // this is a comment\n}";
    cwist_heal_result_t r = cwist_json_heal(input, NULL);
    assert(r.json   != NULL);
    assert(r.healed == true);

    cJSON *parsed = cJSON_Parse(r.json);
    assert(parsed != NULL);
    cJSON *x = cJSON_GetObjectItem(parsed, "x");
    assert(x && x->valueint == 42);
    cJSON_Delete(parsed);

    cwist_heal_result_free(&r);
    printf("  Passed.\n");
}

static void test_l1_bom(void) {
    printf("L1: UTF-8 BOM + trailing comma stripped...\n");
    /* Combine BOM with a trailing-comma error so L1 is definitely needed
     * even if cJSON happens to silently skip the BOM itself. */
    char input[64];
    input[0] = (char)0xEF; input[1] = (char)0xBB; input[2] = (char)0xBF;
    strcpy(input + 3, "{\"ok\":true,}");   /* trailing comma makes it invalid */

    cwist_heal_result_t r = cwist_json_heal(input, NULL);
    assert(r.json   != NULL);
    assert(r.healed == true);
    assert(r.level  == 1);

    cJSON *parsed = cJSON_Parse(r.json);
    assert(parsed != NULL);
    cJSON_Delete(parsed);

    cwist_heal_result_free(&r);
    printf("  Passed.\n");
}

/* ==========================================================================
 * L2 Schema-alignment tests
 * ======================================================================== */

static void test_l2_field_rename(void) {
    printf("L2: field alias 'userId' renamed to 'user_id'...\n");
    /* userId instead of canonical user_id */
    const char *input = "{\"userId\":7,\"name\":\"Bob\"}";
    cwist_heal_config_t cfg = { .threshold = 0.8, .schema = &s_schema };

    cwist_heal_result_t r = cwist_json_heal(input, &cfg);
    assert(r.json   != NULL);
    assert(r.healed == true);
    assert(r.level  == 2);

    cJSON *parsed = cJSON_Parse(r.json);
    assert(parsed != NULL);
    cJSON *uid = cJSON_GetObjectItem(parsed, "user_id");
    assert(uid && uid->valueint == 7);
    /* original alias key must be gone */
    assert(cJSON_GetObjectItem(parsed, "userId") == NULL);
    cJSON_Delete(parsed);

    cwist_heal_result_free(&r);
    printf("  Passed.\n");
}

static void test_l2_type_coercion_str_to_int(void) {
    printf("L2: string '42' coerced to number for user_id...\n");
    const char *input = "{\"user_id\":\"42\",\"name\":\"Carol\"}";
    cwist_heal_config_t cfg = { .threshold = 0.8, .schema = &s_schema };

    cwist_heal_result_t r = cwist_json_heal(input, &cfg);
    assert(r.json   != NULL);
    assert(r.healed == true);
    assert(r.level  == 2);

    cJSON *parsed = cJSON_Parse(r.json);
    assert(parsed != NULL);
    cJSON *uid = cJSON_GetObjectItem(parsed, "user_id");
    assert(uid && cJSON_IsNumber(uid));
    assert(uid->valueint == 42);
    cJSON_Delete(parsed);

    cwist_heal_result_free(&r);
    printf("  Passed.\n");
}

static void test_l2_type_coercion_str_to_bool(void) {
    printf("L2: string 'true' coerced to bool for active...\n");
    const char *input = "{\"user_id\":1,\"name\":\"Dan\",\"active\":\"true\"}";
    cwist_heal_config_t cfg = { .threshold = 0.8, .schema = &s_schema };

    cwist_heal_result_t r = cwist_json_heal(input, &cfg);
    assert(r.json   != NULL);
    assert(r.healed == true);

    cJSON *parsed = cJSON_Parse(r.json);
    assert(parsed != NULL);
    cJSON *act = cJSON_GetObjectItem(parsed, "active");
    assert(act && cJSON_IsTrue(act));
    cJSON_Delete(parsed);

    cwist_heal_result_free(&r);
    printf("  Passed.\n");
}

static void test_l2_fuzzy_match(void) {
    printf("L2: fuzzy 'is_active' matches 'active' schema field...\n");
    const char *input = "{\"user_id\":2,\"name\":\"Eve\",\"is_active\":false}";
    cwist_heal_config_t cfg = { .threshold = 0.8, .schema = &s_schema };

    cwist_heal_result_t r = cwist_json_heal(input, &cfg);
    assert(r.json   != NULL);
    assert(r.healed == true);

    cJSON *parsed = cJSON_Parse(r.json);
    assert(parsed != NULL);
    /* After alignment the canonical name 'active' should be present */
    assert(cJSON_GetObjectItem(parsed, "active") != NULL);
    cJSON_Delete(parsed);

    cwist_heal_result_free(&r);
    printf("  Passed.\n");
}

/* ==========================================================================
 * L3 SLLM callback test
 * ======================================================================== */

static char *mock_sllm(const char *broken_json, const cwist_schema_t *schema,
                        void *userdata) {
    (void)broken_json; (void)schema; (void)userdata;
    /* Simulate SLLM returning a repaired object */
    return strdup("{\"user_id\":99,\"name\":\"SLLM-recovered\"}");
}

static void test_l3_sllm_callback(void) {
    printf("L3: SLLM callback used when L1 fails...\n");
    /* Completely unrecoverable by L1 */
    const char *input = "<<<garbage that no parser can fix>>>";

    cwist_heal_config_t cfg = {
        .threshold     = 0.5,      /* below L3 confidence (0.7) → accepted */
        .schema        = NULL,
        .sllm_fn       = mock_sllm,
        .sllm_userdata = NULL,
    };

    cwist_heal_result_t r = cwist_json_heal(input, &cfg);
    assert(r.json   != NULL);
    assert(r.healed == true);
    assert(r.level  == 3);

    cJSON *parsed = cJSON_Parse(r.json);
    assert(parsed != NULL);
    cJSON *uid = cJSON_GetObjectItem(parsed, "user_id");
    assert(uid && uid->valueint == 99);
    cJSON_Delete(parsed);

    cwist_heal_result_free(&r);
    printf("  Passed.\n");
}

/* ==========================================================================
 * Threshold test
 * ======================================================================== */

static void test_threshold_rejects_low_confidence(void) {
    printf("Threshold: L3 result below threshold returns NULL json...\n");
    const char *input = "not json at all";

    cwist_heal_config_t cfg = {
        .threshold     = 0.9,      /* L3 confidence (0.7) is below this */
        .schema        = NULL,
        .sllm_fn       = mock_sllm,
        .sllm_userdata = NULL,
    };

    cwist_heal_result_t r = cwist_json_heal(input, &cfg);
    assert(r.json == NULL);    /* confidence 0.7 < threshold 0.9 → rejected */
    assert(r.level == 3);      /* we still reached L3 */

    cwist_heal_result_free(&r);
    printf("  Passed.\n");
}

/* ==========================================================================
 * Zod strict validation tests
 * ======================================================================== */

static void test_zod_valid_object(void) {
    printf("Zod: valid object passes...\n");
    const char *raw = "{\"user_id\":1,\"name\":\"Alice\",\"active\":true}";
    cJSON *out = NULL;
    cwist_zod_result_t r = cwist_zod_parse(raw, &s_schema, &out);
    assert(r.valid    == true);
    assert(r.error_count == 0);
    assert(out != NULL);
    cJSON_Delete(out);
    printf("  Passed.\n");
}

static void test_zod_missing_required(void) {
    printf("Zod: missing required field 'name' is rejected...\n");
    const char *raw = "{\"user_id\":1}";
    cJSON *out = NULL;
    cwist_zod_result_t r = cwist_zod_parse(raw, &s_schema, &out);
    assert(r.valid == false);
    assert(r.error_count > 0);
    assert(out == NULL);

    bool found = false;
    for (int i = 0; i < r.error_count; i++) {
        if (strcmp(r.errors[i].field, "name") == 0) { found = true; break; }
    }
    assert(found);
    printf("  Passed.\n");
}

static void test_zod_wrong_type(void) {
    printf("Zod: wrong type (string for INT user_id) is rejected...\n");
    const char *raw = "{\"user_id\":\"not-a-number\",\"name\":\"X\"}";
    cJSON *out = NULL;
    cwist_zod_result_t r = cwist_zod_parse(raw, &s_schema, &out);
    assert(r.valid == false);
    assert(out == NULL);

    bool found = false;
    for (int i = 0; i < r.error_count; i++) {
        if (strcmp(r.errors[i].field, "user_id") == 0) { found = true; break; }
    }
    assert(found);
    printf("  Passed.\n");
}

static void test_zod_invalid_json_syntax(void) {
    printf("Zod: invalid JSON syntax is rejected...\n");
    const char *raw = "{broken}";
    cJSON *out = NULL;
    cwist_zod_result_t r = cwist_zod_parse(raw, &s_schema, &out);
    assert(r.valid == false);
    assert(r.error_count > 0);
    assert(out == NULL);
    printf("  Passed.\n");
}

/* ==========================================================================
 * DB: cwist_db_insert_healed + cwist_db_query_strict
 * ======================================================================== */

static void test_db_insert_healed(void) {
    printf("DB: insert_healed with broken JSON into SQLite...\n");

    cwist_db *db = NULL;
    cwist_error_t err = cwist_db_open(&db, ":memory:");
    assert(err.error.err_i16 == 0 && db != NULL);

    err = cwist_db_exec(db,
        "CREATE TABLE users "
        "(user_id INTEGER, name TEXT, active INTEGER, score REAL);");
    assert(err.error.err_i16 == 0);

    /* Broken: missing closer, userId alias, active as string */
    const char *broken = "{\"userId\":5,\"name\":\"Healed\",\"active\":\"true\"";
    cwist_heal_config_t cfg = { .threshold = 0.8, .schema = &s_schema };

    err = cwist_db_insert_healed(db, "users", broken, &s_schema, &cfg);
    assert(err.error.err_i16 == 0);

    cJSON *rows = NULL;
    err = cwist_db_query(db, "SELECT user_id, name FROM users;", &rows);
    assert(err.error.err_i16 == 0 && rows != NULL);
    assert(cJSON_GetArraySize(rows) == 1);

    cJSON *row  = cJSON_GetArrayItem(rows, 0);
    cJSON *uid  = cJSON_GetObjectItem(row, "user_id");
    cJSON *name = cJSON_GetObjectItem(row, "name");
    assert(uid  && strcmp(uid->valuestring,  "5")      == 0);
    assert(name && strcmp(name->valuestring, "Healed") == 0);

    cJSON_Delete(rows);
    cwist_db_close(db);
    printf("  Passed.\n");
}

static void test_db_insert_healed_rejects_invalid(void) {
    printf("DB: insert_healed rejects completely unrecoverable JSON...\n");

    cwist_db *db = NULL;
    cwist_error_t err = cwist_db_open(&db, ":memory:");
    assert(err.error.err_i16 == 0);

    err = cwist_db_exec(db, "CREATE TABLE t (x INTEGER);");
    assert(err.error.err_i16 == 0);

    const char *garbage = "<<< not json at all >>>";
    err = cwist_db_insert_healed(db, "t", garbage, NULL, NULL);
    assert(err.error.err_i16 == -1);  /* must fail */

    cwist_db_close(db);
    printf("  Passed.\n");
}

static void test_db_query_strict(void) {
    printf("DB: query_strict filters non-conforming rows...\n");

    cwist_db *db = NULL;
    cwist_error_t err = cwist_db_open(&db, ":memory:");
    assert(err.error.err_i16 == 0);

    err = cwist_db_exec(db,
        "CREATE TABLE users (user_id INTEGER, name TEXT);"
        "INSERT INTO users VALUES (1, 'Alice');"
        "INSERT INTO users VALUES (2, 'Bob');");
    assert(err.error.err_i16 == 0);

    /* Schema: user_id (INT, required) + name (STRING, required) */
    static const cwist_schema_field_t qfields[] = {
        { "user_id", {NULL}, CWIST_FIELD_INT,    true },
        { "name",    {NULL}, CWIST_FIELD_STRING, true },
    };
    static const cwist_schema_t qschema = { qfields, 2 };

    cJSON *rows = NULL;
    /* SQLite returns numbers as strings via exec callback, so all fields will
     * be strings here — query_strict's Zod check on the raw result will flag
     * user_id as wrong type.  This test verifies the filtering logic runs.
     * Both rows are returned when no schema is given. */
    err = cwist_db_query(db, "SELECT user_id, name FROM users;", &rows);
    assert(err.error.err_i16 == 0);
    assert(cJSON_GetArraySize(rows) == 2);
    cJSON_Delete(rows);

    /* With strict schema (user_id must be INT) the sqlite exec callback
     * returns user_id as a STRING, so both rows are filtered out. */
    cJSON *strict_rows = NULL;
    err = cwist_db_query_strict(db, "SELECT user_id, name FROM users;",
                                 &strict_rows, &qschema);
    assert(err.error.err_i16 == 0);
    /* exec callback gives all values as strings → user_id type mismatch */
    assert(cJSON_GetArraySize(strict_rows) == 0);
    cJSON_Delete(strict_rows);

    cwist_db_close(db);
    printf("  Passed.\n");
}

/* ==========================================================================
 * main
 * ======================================================================== */

int main(void) {
    printf("=== L1 Syntax Healing ===\n");
    test_l1_already_valid();
    test_l1_trailing_comma();
    test_l1_missing_closers();
    test_l1_line_comment();
    test_l1_bom();

    printf("\n=== L2 Schema Alignment ===\n");
    test_l2_field_rename();
    test_l2_type_coercion_str_to_int();
    test_l2_type_coercion_str_to_bool();
    test_l2_fuzzy_match();

    printf("\n=== L3 SLLM Callback ===\n");
    test_l3_sllm_callback();

    printf("\n=== Threshold Enforcement ===\n");
    test_threshold_rejects_low_confidence();

    printf("\n=== Zod Strict Validation ===\n");
    test_zod_valid_object();
    test_zod_missing_required();
    test_zod_wrong_type();
    test_zod_invalid_json_syntax();

    printf("\n=== DB Integration ===\n");
    test_db_insert_healed();
    test_db_insert_healed_rejects_invalid();
    test_db_query_strict();

    printf("\nAll json_heal / zod / DB tests passed!\n");
    return 0;
}
