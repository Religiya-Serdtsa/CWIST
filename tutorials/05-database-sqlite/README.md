# Tutorial 05: Embedded SQLite Database Integration

Initialize an embedded in-memory SQLite database, run SQL statements, and convert query results directly into JSON.

## Key Concepts
- Opening SQLite connections with `cwist_db_open(&db, ":memory:")`.
- Executing queries and serializing data with `cwist_db_query(db, sql, &cjson_result)`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut05
```

Test with `curl`:
```bash
curl http://127.0.0.1:8084/db
```
