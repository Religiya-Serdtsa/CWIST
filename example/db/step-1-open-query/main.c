/**
 * @file main.c
 * @brief ORM open, insert and select via the socket-backed ORM API.
 *
 * Instead of raw sqlite3 calls, we spawn a SQLite worker behind a Unix
 * socket and speak to it through cwist_orm_* helpers.
 */

#include <stdio.h>
#include <cwist/app.h>
#include <cwist/core/orm/orm.h>
#include <cwist/core/orm/orm_socket.h>
#include <cjson/cJSON.h>

int main(void) {
    printf("=== ORM: Open, Insert & Select ===\n");

    int sock = cwist_db_transfer_sqlite_to_socket(":memory:");
    cwist_orm_t *orm = cwist_orm_open_socket(sock);
    cwist_orm_immediate_commit(true);

    cwist_orm_exec(orm,
        "CREATE TABLE users ("
        "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  age  INTEGER"
        ");");
    printf("Table 'users' created\n");

    cJSON *alice = cJSON_Parse("{\"name\":\"Alice\",\"age\":30}");
    cJSON *bob   = cJSON_Parse("{\"name\":\"Bob\",\"age\":25}");
    cJSON *carol = cJSON_Parse("{\"name\":\"Carol\",\"age\":35}");
    cwist_orm_insert(orm, "users", alice);
    cwist_orm_insert(orm, "users", bob);
    cwist_orm_insert(orm, "users", carol);
    cJSON_Delete(alice);
    cJSON_Delete(bob);
    cJSON_Delete(carol);
    printf("3 rows inserted via ORM\n");

    printf("\n[SELECT]\n");
    cJSON *rows = NULL;
    cwist_orm_select(orm, "users", "id, name, age", NULL, &rows);
    if (rows) {
        int n = cJSON_GetArraySize(rows);
        for (int i = 0; i < n; i++) {
            cJSON *row  = cJSON_GetArrayItem(rows, i);
            cJSON *id   = cJSON_GetObjectItem(row, "id");
            cJSON *name = cJSON_GetObjectItem(row, "name");
            cJSON *age  = cJSON_GetObjectItem(row, "age");
            printf("  id=%-3s  name=%-8s  age=%s\n",
                (id   && id->valuestring)   ? id->valuestring   : "?",
                (name && name->valuestring) ? name->valuestring : "?",
                (age  && age->valuestring)  ? age->valuestring  : "?");
        }
        cJSON_Delete(rows);
    }

    cwist_orm_close_socket(orm);
    printf("\n=== Done ===\n");
    return 0;
}
