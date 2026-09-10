#define _POSIX_C_SOURCE 200809L
/* cwist_db_open_memory / cwist_db_serialize: blob-backed databases. */
#include <cwist/core/db/sql.h>
#include <cwist/core/mem/alloc.h>
#include <cjson/cJSON.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void assert_names(cwist_db *db, const char *expected_csv) {
    cJSON *result = NULL;
    cwist_error_t err = cwist_db_query(db, "SELECT name FROM t ORDER BY id;", &result);
    assert(err.error.err_i16 == 0);
    assert(result != NULL && cJSON_IsArray(result));
    size_t cap = strlen(expected_csv) + 1;
    char *csv = (char *)cwist_alloc(cap);
    assert(csv != NULL);
    csv[0] = '\0';
    cJSON *row;
    cJSON_ArrayForEach(row, result) {
        cJSON *name = cJSON_GetObjectItem(row, "name");
        assert(name != NULL && cJSON_IsString(name));
        if (csv[0]) strncat(csv, ",", cap - strlen(csv) - 1);
        strncat(csv, name->valuestring, cap - strlen(csv) - 1);
    }
    assert(strcmp(csv, expected_csv) == 0);
    cwist_free(csv);
    cJSON_Delete(result);
}

int main(void) {
    /* Build a source image in a scratch in-memory db. */
    cwist_db *src = NULL;
    assert(cwist_db_open(&src, ":memory:").error.err_i16 == 0);
    assert(cwist_db_exec(src, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT);").error.err_i16 == 0);
    assert(cwist_db_exec(src, "INSERT INTO t (name) VALUES ('alpha'), ('beta');").error.err_i16 == 0);

    void *blob = NULL;
    size_t blob_len = 0;
    assert(cwist_db_serialize(src, &blob, &blob_len).error.err_i16 == 0);
    assert(blob != NULL && blob_len > 0);
    assert(memcmp(blob, "SQLite format 3\0", 16) == 0);
    cwist_db_close(src);

    /* Read-only open: queries work, writes are rejected. */
    cwist_db *ro = NULL;
    assert(cwist_db_open_memory(&ro, blob, (size_t)blob_len, 1).error.err_i16 == 0);
    assert_names(ro, "alpha,beta");
    cwist_error_t we = cwist_db_exec(ro, "INSERT INTO t (name) VALUES ('nope');");
    assert(!cwist_error_is_ok(&we));
    cwist_error_dispose(&we);
    cwist_db_close(ro);

    /* Read-write open: the caller's buffer is untouched by writes. */
    cwist_db *rw = NULL;
    assert(cwist_db_open_memory(&rw, blob, (size_t)blob_len, 0).error.err_i16 == 0);
    assert(cwist_db_exec(rw, "INSERT INTO t (name) VALUES ('gamma');").error.err_i16 == 0);
    assert_names(rw, "alpha,beta,gamma");

    /* Serialize round-trip: the write survives into a new in-memory db. */
    void *blob2 = NULL;
    size_t blob2_len = 0;
    assert(cwist_db_serialize(rw, &blob2, &blob2_len).error.err_i16 == 0);
    cwist_db_close(rw);

    cwist_db *rw2 = NULL;
    assert(cwist_db_open_memory(&rw2, blob2, blob2_len, 0).error.err_i16 == 0);
    assert_names(rw2, "alpha,beta,gamma");
    cwist_db_close(rw2);

    /* Original blob was copied on open: still byte-identical and reusable. */
    cwist_db *again = NULL;
    assert(cwist_db_open_memory(&again, blob, (size_t)blob_len, 0).error.err_i16 == 0);
    assert_names(again, "alpha,beta");
    cwist_db_close(again);

    /* Bad input rejected. */
    cwist_db *bad = NULL;
    assert(cwist_db_open_memory(&bad, NULL, 10, 0).error.err_i16 != 0);
    assert(bad == NULL);
    assert(cwist_db_open_memory(&bad, blob, 0, 0).error.err_i16 != 0);
    assert(bad == NULL);

    cwist_free(blob);
    cwist_free(blob2);
    printf("test_db_memory: OK\n");
    return 0;
}
