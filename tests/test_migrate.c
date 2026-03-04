/**
 * @file test_migrate.c
 * @brief Unit tests for the DB migration and db_crypt modules.
 */

#include <cwist/core/db/migrate.h>
#include <cwist/security/db_crypt/db_crypt.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Migration tests
 * ---------------------------------------------------------------------- */

static const cwist_migration_t migrations[] = {
    {
        1,
        "create_users",
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL);",
        "DROP TABLE IF EXISTS users;"
    },
    {
        2,
        "add_email",
        "ALTER TABLE users ADD COLUMN email TEXT;",
        NULL /* SQLite doesn't support DROP COLUMN in old versions */
    },
    {
        3,
        "create_sessions",
        "CREATE TABLE sessions (id TEXT PRIMARY KEY, user_id INTEGER);",
        "DROP TABLE IF EXISTS sessions;"
    },
};
static const int N_MIGRATIONS = 3;

static void test_migrate_up_and_version(void) {
    printf("Testing migrate up and version...\n");

    sqlite3 *db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);

    assert(cwist_migrate_version(db) == 0);

    int rc = cwist_migrate_up(db, migrations, N_MIGRATIONS);
    assert(rc == CWIST_MIGRATE_OK);
    assert(cwist_migrate_version(db) == 3);

    /* Verify tables exist. */
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN "
        "('users','sessions');", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 2);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    printf("  Passed.\n");
}

static void test_migrate_idempotent(void) {
    printf("Testing migrate up is idempotent...\n");

    sqlite3 *db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);

    assert(cwist_migrate_up(db, migrations, N_MIGRATIONS) == CWIST_MIGRATE_OK);
    assert(cwist_migrate_up(db, migrations, N_MIGRATIONS) == CWIST_MIGRATE_OK);
    assert(cwist_migrate_version(db) == 3);

    sqlite3_close(db);
    printf("  Passed.\n");
}

static void test_migrate_down_one(void) {
    printf("Testing migrate down 1 step...\n");

    sqlite3 *db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);

    assert(cwist_migrate_up(db, migrations, N_MIGRATIONS) == CWIST_MIGRATE_OK);
    assert(cwist_migrate_version(db) == 3);

    /* Roll back 1 step: removes sessions table. */
    int rc = cwist_migrate_down(db, migrations, N_MIGRATIONS, 1);
    assert(rc == CWIST_MIGRATE_OK);
    assert(cwist_migrate_version(db) == 2);

    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='sessions';",
        -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 0); /* sessions table gone */
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    printf("  Passed.\n");
}

static void test_migrate_down_all(void) {
    printf("Testing migrate down all steps...\n");

    sqlite3 *db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);

    assert(cwist_migrate_up(db, migrations, N_MIGRATIONS) == CWIST_MIGRATE_OK);

    int rc = cwist_migrate_down(db, migrations, N_MIGRATIONS, 0);
    assert(rc == CWIST_MIGRATE_OK);

    /* version 2 has no down_sql so it's left in the history but not reverted.
     * versions 1 and 3 have down_sql. After full rollback version should be 2
     * (since v2's down_sql is NULL it stays in history). */
    /* sessions (v3) rolled back, users (v1) rolled back, email col stays */
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='sessions';",
        -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 0);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    printf("  Passed.\n");
}

/* -------------------------------------------------------------------------
 * db_crypt tests
 * ---------------------------------------------------------------------- */

static void test_db_crypt_roundtrip(void) {
    printf("Testing db_crypt seal/open roundtrip...\n");

    cwist_db_crypt_ctx_t ctx;
    /* Fill KEK with a deterministic test value. */
    memset(ctx.kek, 0xAB, CWIST_DB_CRYPT_KEY_LEN);

    const unsigned char plaintext[] = "Hello, SQLite world! This is a test database blob.";
    size_t pt_len = sizeof(plaintext) - 1;

    size_t blob_len = 0;
    unsigned char *blob = cwist_db_crypt_seal(&ctx, plaintext, pt_len, &blob_len);
    assert(blob != NULL);
    assert(blob_len > pt_len);

    size_t out_len = 0;
    unsigned char *recovered = cwist_db_crypt_open(&ctx, blob, blob_len, &out_len);
    assert(recovered != NULL);
    assert(out_len == pt_len);
    assert(memcmp(recovered, plaintext, pt_len) == 0);

    free(blob);
    free(recovered);
    printf("  Passed.\n");
}

static void test_db_crypt_wrong_kek(void) {
    printf("Testing db_crypt rejects wrong KEK...\n");

    cwist_db_crypt_ctx_t ctx_enc, ctx_dec;
    memset(ctx_enc.kek, 0x11, CWIST_DB_CRYPT_KEY_LEN);
    memset(ctx_dec.kek, 0x22, CWIST_DB_CRYPT_KEY_LEN); /* different */

    const unsigned char data[] = "secret data";
    size_t blob_len = 0;
    unsigned char *blob = cwist_db_crypt_seal(&ctx_enc, data, sizeof(data)-1, &blob_len);
    assert(blob != NULL);

    size_t out_len = 0;
    unsigned char *recovered = cwist_db_crypt_open(&ctx_dec, blob, blob_len, &out_len);
    /* With wrong KEK the DEK will decrypt to garbage; decryption should fail
     * or return incorrect data. Either way we must not get the original back. */
    if (recovered) {
        assert(out_len != sizeof(data)-1 ||
               memcmp(recovered, data, sizeof(data)-1) != 0);
        free(recovered);
    }
    free(blob);
    printf("  Passed.\n");
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(void) {
    printf("=== Migration tests ===\n");
    test_migrate_up_and_version();
    test_migrate_idempotent();
    test_migrate_down_one();
    test_migrate_down_all();

    printf("=== DB Crypt tests ===\n");
    test_db_crypt_roundtrip();
    test_db_crypt_wrong_kek();

    printf("\nAll tests passed.\n");
    return 0;
}
