# Database API

*Header:* `<cwist/core/db/sql.h>`

Wrapper for SQLite3 database operations.

### `cwist_db_open`
```c
cwist_error_t cwist_db_open(cwist_db **db, const char *path);
```
Opens a connection to a SQLite database file.

### `cwist_db_exec`
```c
cwist_error_t cwist_db_exec(cwist_db *db, const char *sql);
```
Executes a non-query SQL command (INSERT, UPDATE, DELETE). Returns `err_i16 = -1`
when called with a NULL handle or SQL pointer so callers can safely guard inputs.

### `cwist_db_query`
```c
cwist_error_t cwist_db_query(cwist_db *db, const char *sql, cJSON **result);
```
Executes a SELECT query. `result` is populated with a cJSON Array of Objects on
success and reset to `NULL` if validation or SQLite execution fails.

Integrate the handle with the framework via `cwist_app_use_db(app, "app.db");` — every incoming `cwist_http_request` then exposes the pointer at `req->db`.
