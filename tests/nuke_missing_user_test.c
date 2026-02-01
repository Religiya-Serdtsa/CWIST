#include <assert.h>
#include <cwist/core/db/nuke_db.h>
#include <cwist/core/db/sql.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>

static void test_missing_user_lookup(cwist_db *db) {
    cJSON *result = NULL;
    cwist_error_t err = cwist_db_query(db, "SELECT username FROM users WHERE id = -1;", &result);
    assert(err.error.err_i16 == 0);
    assert(result != NULL);
    assert(cJSON_GetArraySize(result) == 0);
    cJSON_Delete(result);
}

static void test_missing_table_query(cwist_db *db) {
    cJSON *result = NULL;
    cwist_error_t err = cwist_db_query(db, "SELECT * FROM __cwist_missing_table;", &result);
    assert(err.errtype == CWIST_ERR_JSON);
    assert(result == NULL);
}

static void test_null_db_guards(void) {
    cJSON *result = NULL;
    cwist_error_t query_err = cwist_db_query(NULL, "SELECT 1;", &result);
    assert(query_err.error.err_i16 == -1);
    assert(result == NULL);

    cwist_error_t exec_err = cwist_db_exec(NULL, "CREATE TABLE x(id INT);");
    assert(exec_err.error.err_i16 == -1);
}

static void test_integrity_check(cwist_db *db) {
    cJSON *result = NULL;
    cwist_error_t err = cwist_db_query(db, "PRAGMA integrity_check;", &result);
    assert(err.error.err_i16 == 0);
    assert(result != NULL);
    assert(cJSON_GetArraySize(result) == 1);
    cJSON *row = cJSON_GetArrayItem(result, 0);
    assert(row != NULL);
    cJSON *val = cJSON_GetObjectItem(row, "integrity_check");
    assert(val && val->valuestring);
    assert(strcmp(val->valuestring, "ok") == 0);
    cJSON_Delete(result);
}

int main(void) {
    const char *db_path = "ceversi/othello.db";
    if (cwist_nuke_init(db_path, 0) != 0) {
        fprintf(stderr, "cwist_nuke_init failed\n");
        return 1;
    }

    sqlite3 *handle = cwist_nuke_get_db();
    if (!handle) {
        fprintf(stderr, "cwist_nuke_get_db returned NULL\n");
        cwist_nuke_close();
        return 1;
    }

    cwist_db db = {0};
    db.conn = handle;

    test_missing_user_lookup(&db);
    test_missing_table_query(&db);
    test_integrity_check(&db);
    test_null_db_guards();

    printf("[NukeDB] Missing-record safety tests passed.\n");

    cwist_nuke_close();
    return 0;
}
