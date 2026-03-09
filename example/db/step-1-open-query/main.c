#include <stdio.h>
#include <cwist/core/db/sql.h>
#include <cjson/cJSON.h>

int main() {
    printf("=== SQLite: Open & Query ===\n");

    /* 1. Open an in-memory database */
    cwist_db *db = NULL;
    cwist_error_t err = cwist_db_open(&db, ":memory:");
    if (err.error.err_i16 != 0) {
        fprintf(stderr, "Failed to open database\n");
        return 1;
    }
    printf("Database opened (in-memory)\n");

    /* 2. Create a table */
    printf("\n[CREATE TABLE]\n");
    err = cwist_db_exec(db,
        "CREATE TABLE users ("
        "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  age  INTEGER"
        ");");
    if (err.error.err_i16 != 0) {
        fprintf(stderr, "CREATE TABLE failed\n");
        cwist_db_close(db);
        return 1;
    }
    printf("Table 'users' created\n");

    /* 3. Insert rows */
    printf("\n[INSERT]\n");
    cwist_db_exec(db, "INSERT INTO users (name, age) VALUES ('Alice', 30);");
    cwist_db_exec(db, "INSERT INTO users (name, age) VALUES ('Bob',   25);");
    cwist_db_exec(db, "INSERT INTO users (name, age) VALUES ('Carol', 35);");
    printf("3 rows inserted\n");

    /* 4. Query rows */
    printf("\n[SELECT]\n");
    cJSON *rows = NULL;
    err = cwist_db_query(db, "SELECT id, name, age FROM users ORDER BY age;", &rows);
    if (err.error.err_i16 != 0 || !rows) {
        fprintf(stderr, "Query failed\n");
        cwist_db_close(db);
        return 1;
    }

    int n = cJSON_GetArraySize(rows);
    for (int i = 0; i < n; i++) {
        cJSON *row  = cJSON_GetArrayItem(rows, i);
        cJSON *id   = cJSON_GetObjectItem(row, "id");
        cJSON *name = cJSON_GetObjectItem(row, "name");
        cJSON *age  = cJSON_GetObjectItem(row, "age");
        /* cwist_db_query stores all values as strings */
        printf("  id=%-3s  name=%-8s  age=%s\n",
            (id   && id->valuestring)   ? id->valuestring   : "?",
            (name && name->valuestring) ? name->valuestring : "?",
            (age  && age->valuestring)  ? age->valuestring  : "?");
    }
    cJSON_Delete(rows);

    /* 5. Close */
    cwist_db_close(db);
    printf("\n=== Done ===\n");
    return 0;
}
