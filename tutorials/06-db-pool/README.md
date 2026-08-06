# Tutorial 06: High-Concurrency Database Connection Pool

Manage thread-safe SQLite connection allocation under multi-threaded workload.

## Key Concepts
- Initialize a connection pool with `cwist_db_pool_create(path, max_conns)`.
- Acquire and release connections safely using `cwist_db_pool_acquire` and `cwist_db_pool_release`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut06
```

Test with `curl`:
```bash
curl http://127.0.0.1:8085/pool
```
