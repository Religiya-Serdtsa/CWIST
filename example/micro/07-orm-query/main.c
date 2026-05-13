/**
 * @file main.c
 * @brief 07-orm-query — SELECT via ORM.
 */

#include <stdio.h>
#include <cwist/core/orm/orm.h>
#include <cwist/core/orm/orm_socket.h>
#include <cjson/cJSON.h>

int main(void) {
    int sock = cwist_db_transfer_sqlite_to_socket(":memory:");
    cwist_orm_t *orm = cwist_orm_open_socket(sock);
    cwist_orm_immediate_commit(true);

    cwist_orm_exec(orm, "CREATE TABLE posts(id INTEGER PRIMARY KEY, title TEXT);");
    cJSON *r1 = cJSON_Parse("{\"title\":\"First\"}");
    cJSON *r2 = cJSON_Parse("{\"title\":\"Second\"}");
    cwist_orm_insert(orm, "posts", r1);
    cwist_orm_insert(orm, "posts", r2);
    cJSON_Delete(r1);
    cJSON_Delete(r2);

    cJSON *rows = NULL;
    cwist_orm_select(orm, "posts", "*", NULL, &rows);
    if (rows) {
        int n = cJSON_GetArraySize(rows);
        for (int i = 0; i < n; i++) {
            cJSON *row = cJSON_GetArrayItem(rows, i);
            cJSON *t = cJSON_GetObjectItem(row, "title");
            printf("  %s\n", t ? t->valuestring : "?");
        }
        cJSON_Delete(rows);
    }

    cwist_orm_close_socket(orm);
    return 0;
}
