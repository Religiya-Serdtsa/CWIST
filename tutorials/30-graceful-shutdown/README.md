# Tutorial 30: Graceful Server Shutdown

Catch system termination signals (`SIGTERM`, `SIGINT`) to drain active connections and close database handles gracefully.

## Key Concepts
- Registering signal handlers and initiating graceful teardown.
- `cwist_full_gc(true)` (`cwist/core/mem/gc.h`) as the automatic half of
  graceful shutdown: a worker thread or process that ends without running
  the explicit `cwist_app_destroy()` path still gets its connections closed
  and its `cwist_alloc()` blocks reclaimed (per-thread TLS sweep on thread
  exit, `atexit` sweep on process exit). It's a one-shot, process-wide
  toggle — first call wins, every later call is a no-op — and it's off by
  default, so the explicit model runs unchanged at zero cost if you never
  call it. Call it alongside `cwist_app_destroy()`, not instead of it: the
  explicit path stays the deterministic primary shutdown, full-GC is the
  safety net for the paths that skip it. See `docs/GC.md`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut30
```
