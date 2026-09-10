# Database API

*Header:* `<cwist/core/db/sql.h>`

Wrapper for SQLite3 database operations.

### `cwist_db_open`
```c
cwist_error_t cwist_db_open(cwist_db **db, const char *path);
```
Opens a connection to a SQLite database file.

### `cwist_db_open_memory`
```c
cwist_error_t cwist_db_open_memory(cwist_db **db, const void *buf, size_t len, int readonly);
```
Opens a database from an in-memory SQLite image (e.g. a blob fetched by a WASM
or edge host) via `sqlite3_deserialize`. The image is copied, so the caller
keeps ownership of `buf`. `readonly != 0` rejects writes with `SQLITE_READONLY`.

### `cwist_db_serialize`
```c
cwist_error_t cwist_db_serialize(cwist_db *db, void **out, size_t *out_len);
```
Serializes the database into a freshly allocated image buffer (free with
`cwist_free()`), suitable for persisting back to a blob or for handing to
`cwist_db_open_memory()`.

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
