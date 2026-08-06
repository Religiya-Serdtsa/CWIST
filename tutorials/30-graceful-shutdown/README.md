# Tutorial 30: Graceful Server Shutdown

Catch system termination signals (`SIGTERM`, `SIGINT`) to drain active connections and close database handles gracefully.

## Key Concepts
- Registering signal handlers and initiating graceful teardown.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut30
```
