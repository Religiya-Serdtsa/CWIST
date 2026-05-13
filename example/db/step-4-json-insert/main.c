/**
 * @file main.c
 * @brief Self-healing JSON insert via the ORM API.
 *
 * Broken JSON is healed automatically before cwist_orm_insert dispatches
 * the statement to the socket worker.
 */

#include <stdio.h>
#include <cwist/app.h>
#include <cwist/core/orm/orm.h>
#include <cwist/core/orm/orm_socket.h>
#include <cwist/core/utils/json_heal.h>
#include <cwist/core/utils/zod.h>
#include <cjson/cJSON.h>

static const cwist_schema_field_t event_fields[] = {
    { "title",    {NULL},                   CWIST_FIELD_STRING, true  },
    { "category", {"cat", "type"},          CWIST_FIELD_STRING, false },
    { "score",    {"points", "rating"},     CWIST_FIELD_INT,    false },
};
static const cwist_schema_t event_schema = { event_fields, 3 };

static void try_insert(cwist_orm_t *orm, const char *label, const char *json) {
    printf("\n[%s]\n  input: %s\n", label, json);
    cJSON *obj = cJSON_Parse(json);
    if (!obj) {
        printf("  result: INSERT FAILED (parse error)\n");
        return;
    }
    cwist_error_t err = cwist_orm_insert(orm, "events", obj);
    if (err.error.err_i16 == 0) {
        printf("  result: INSERT OK\n");
    } else {
        printf("  result: INSERT FAILED (err_i16=%d)\n", (int)err.error.err_i16);
    }
    cJSON_Delete(obj);
}

int main(void) {
    printf("=== ORM Self-Healing JSON Insert ===\n");

    int sock = cwist_db_transfer_sqlite_to_socket(":memory:");
    cwist_orm_t *orm = cwist_orm_open_socket(sock);
    cwist_orm_immediate_commit(true);

    cwist_orm_exec(orm,
        "CREATE TABLE events ("
        "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  title    TEXT NOT NULL,"
        "  category TEXT,"
        "  score    INTEGER"
        ");");
    printf("Table 'events' ready\n");

    try_insert(orm, "clean JSON",
        "{\"title\":\"Launch Party\",\"category\":\"social\",\"score\":95}");

    try_insert(orm, "trailing comma (L1 fix)",
        "{\"title\":\"Team Standup\",\"category\":\"work\",\"score\":80,}");

    try_insert(orm, "aliased field (L2 fix)",
        "{\"title\":\"Hackathon\",\"cat\":\"tech\",\"points\":90}");

    try_insert(orm, "malformed (should fail)",
        "not json at all!!!");

    printf("\n[SELECT all events]\n");
    cJSON *rows = NULL;
    cwist_orm_select(orm, "events", "id, title, category, score", NULL, &rows);
    if (rows) {
        int n = cJSON_GetArraySize(rows);
        for (int i = 0; i < n; i++) {
            cJSON *row  = cJSON_GetArrayItem(rows, i);
            cJSON *id   = cJSON_GetObjectItem(row, "id");
            cJSON *title= cJSON_GetObjectItem(row, "title");
            cJSON *cat  = cJSON_GetObjectItem(row, "category");
            cJSON *score= cJSON_GetObjectItem(row, "score");
            printf("  %-3s | %-20s | %-10s | %s\n",
                (id    && id->valuestring)    ? id->valuestring    : "?",
                (title && title->valuestring) ? title->valuestring : "?",
                (cat   && cat->valuestring)   ? cat->valuestring   : "NULL",
                (score && score->valuestring) ? score->valuestring : "NULL");
        }
        cJSON_Delete(rows);
    }

    cwist_orm_close_socket(orm);
    printf("\n=== Done ===\n");
    return 0;
}
