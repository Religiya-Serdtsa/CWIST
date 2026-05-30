/**
 * @file main.c
 * @brief 08-orm-update-delete — modify and remove rows.
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
    cJSON *r = cJSON_Parse("{\"title\":\"Old Title\"}");
    cwist_orm_insert(orm, "posts", r);
    cJSON_Delete(r);

    cJSON *upd = cJSON_Parse("{\"title\":\"New Title\"}");
    cwist_orm_update(orm, "posts", upd, "id = 1");
    cJSON_Delete(upd);
    printf("Updated row 1\n");

    cwist_orm_delete(orm, "posts", "id = 1");
    printf("Deleted row 1\n");

    cwist_orm_close_socket(orm);
    return 0;
}
