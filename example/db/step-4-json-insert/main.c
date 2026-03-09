#include <stdio.h>
#include <cwist/core/db/sql.h>
#include <cwist/core/utils/json_heal.h>
#include <cwist/core/utils/zod.h>
#include <cjson/cJSON.h>

/* Schema for the 'events' table */
static const cwist_schema_field_t event_fields[] = {
    { "title",    {NULL},                   CWIST_FIELD_STRING, true  },
    { "category", {"cat", "type"},          CWIST_FIELD_STRING, false },
    { "score",    {"points", "rating"},     CWIST_FIELD_INT,    false },
};
static const cwist_schema_t event_schema = { event_fields, 3 };

static void try_insert(cwist_db *db, const char *label, const char *json) {
    printf("\n[%s]\n  input: %s\n", label, json);
    cwist_error_t err = cwist_db_insert_healed(db, "events", json, &event_schema, NULL);
    if (err.error.err_i16 == 0) {
        printf("  result: INSERT OK\n");
    } else {
        printf("  result: INSERT FAILED (err_i16=%d)\n", (int)err.error.err_i16);
    }
}

int main() {
    printf("=== Self-Healing JSON Insert ===\n");

    cwist_db *db = NULL;
    cwist_db_open(&db, ":memory:");
    cwist_db_exec(db,
        "CREATE TABLE events ("
        "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  title    TEXT NOT NULL,"
        "  category TEXT,"
        "  score    INTEGER"
        ");");
    printf("Table 'events' ready\n");

    /* 1. Clean JSON — inserted directly */
    try_insert(db, "clean JSON",
        "{\"title\":\"Launch Party\",\"category\":\"social\",\"score\":95}");

    /* 2. Broken JSON (trailing comma) — L1 healing fixes it */
    try_insert(db, "trailing comma (L1 fix)",
        "{\"title\":\"Team Standup\",\"category\":\"work\",\"score\":80,}");

    /* 3. Aliased field name 'cat' → 'category' — L2 alignment */
    try_insert(db, "aliased field (L2 fix)",
        "{\"title\":\"Hackathon\",\"cat\":\"tech\",\"points\":90}");

    /* 4. Completely malformed — should fail */
    try_insert(db, "malformed (should fail)",
        "not json at all!!!");

    /* 5. Query results */
    printf("\n[SELECT all events]\n");
    cJSON *rows = NULL;
    cwist_db_query(db, "SELECT id, title, category, score FROM events;", &rows);
    if (rows) {
        int n = cJSON_GetArraySize(rows);
        for (int i = 0; i < n; i++) {
            cJSON *row  = cJSON_GetArrayItem(rows, i);
            cJSON *id   = cJSON_GetObjectItem(row, "id");
            cJSON *title= cJSON_GetObjectItem(row, "title");
            cJSON *cat  = cJSON_GetObjectItem(row, "category");
            cJSON *score= cJSON_GetObjectItem(row, "score");
            /* cwist_db_query stores all values as strings */
            printf("  %-3s | %-20s | %-10s | %s\n",
                (id    && id->valuestring)    ? id->valuestring    : "?",
                (title && title->valuestring) ? title->valuestring : "?",
                (cat   && cat->valuestring)   ? cat->valuestring   : "NULL",
                (score && score->valuestring) ? score->valuestring : "NULL");
        }
        cJSON_Delete(rows);
    }

    cwist_db_close(db);
    printf("\n=== Done ===\n");
    return 0;
}
