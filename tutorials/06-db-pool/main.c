#include <cwist/app.h>
#include <cwist/core/db/pool.h>

static cwist_db_pool_t *g_pool = NULL;

static void handle_pool(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_db *db = cwist_db_pool_acquire(g_pool);
    if (!db) {
        cwist_sstring_assign(res->body, "Connection pool acquire timeout");
        return;
    }

    cJSON *result = NULL;
    cwist_db_query(db, "SELECT 'Pooled Connection OK' AS message;", &result);
    char *json_str = cJSON_PrintUnformatted(result);

    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, json_str);

    free(json_str);
    cJSON_Delete(result);
    cwist_db_pool_release(g_pool, db);
}

int main(void) {
    g_pool = cwist_db_pool_create(":memory:", 8);

    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/pool", handle_pool);
    cwist_app_listen(app, 8085);

    cwist_app_destroy(app);
    cwist_db_pool_destroy(g_pool);
    return 0;
}
