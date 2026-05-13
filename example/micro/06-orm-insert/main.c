/**
 * @file main.c
 * @brief 06-orm-insert — insert rows via ORM.
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

    cJSON *row = cJSON_Parse("{\"title\":\"Hello ORM\"}");
    cwist_orm_insert(orm, "posts", row);
    cJSON_Delete(row);

    printf("Inserted 1 row\n");
    cwist_orm_close_socket(orm);
    return 0;
}
