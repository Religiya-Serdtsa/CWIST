#include <cwist/app.h>
#include <cwist/core/db/db.h>

static void handle_db(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_db *db = NULL;
    if (cwist_db_open(&db, ":memory:").error.err_i16 != 0) {
        cwist_sstring_assign(res->body, "Failed to open in-memory database");
        return;
    }

    cwist_db_exec(db, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);");
    cwist_db_exec(db, "INSERT INTO users (name) VALUES ('Alice');");

    cJSON *result = NULL;
    cwist_db_query(db, "SELECT * FROM users;", &result);
    char *json_str = cJSON_PrintUnformatted(result);

    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, json_str);

    free(json_str);
    cJSON_Delete(result);
    cwist_db_close(db);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/db", handle_db);
    cwist_app_listen(app, 8084);
    cwist_app_destroy(app);
    return 0;
}
