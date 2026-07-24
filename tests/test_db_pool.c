#define _POSIX_C_SOURCE 200809L
#include <cwist/core/db/pool.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void) {
    cwist_db_pool_t *pool = cwist_db_pool_create(":memory:", 3);
    assert(pool != NULL);

    cwist_error_t err = cwist_db_pool_exec(pool,
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);");
    assert(err.error.err_i16 == 0);

    err = cwist_db_pool_exec(pool,
        "INSERT INTO users (name) VALUES ('alice'), ('bob');");
    assert(err.error.err_i16 == 0);

    cJSON *result = NULL;
    err = cwist_db_pool_query(pool, "SELECT * FROM users ORDER BY id;", &result);
    assert(err.error.err_i16 == 0);
    assert(result != NULL);
    assert(cJSON_IsArray(result));
    assert(cJSON_GetArraySize(result) == 2);

    cJSON *first = cJSON_GetArrayItem(result, 0);
    cJSON *name = cJSON_GetObjectItem(first, "name");
    assert(name != NULL && cJSON_IsString(name));
    assert(strcmp(name->valuestring, "alice") == 0);

    cJSON_Delete(result);

    /* Manual acquire/release smoke test. */
    cwist_db *conn = cwist_db_pool_acquire(pool);
    assert(conn != NULL);
    cwist_db_pool_release(pool, conn);

    /* Every connection shares the :memory: database and timeout is bounded. */
    conn = cwist_db_pool_acquire(pool);
    assert(conn != NULL);
    assert(cwist_db_pool_in_use(pool) == 1);
    cwist_db_pool_release(pool, conn);

    cwist_db *one = cwist_db_pool_acquire(pool);
    cwist_db *two = cwist_db_pool_acquire(pool);
    cwist_db *three = cwist_db_pool_acquire(pool);
    assert(one && two && three);
    assert(cwist_db_pool_in_use(pool) == 3);
    assert(cwist_db_pool_acquire_timeout(pool, 5) == NULL);
    cwist_db_pool_release(pool, one); cwist_db_pool_release(pool, two); cwist_db_pool_release(pool, three);

    cwist_db_pool_destroy(pool);
    printf("All DB pool tests passed.\n");
    return 0;
}
