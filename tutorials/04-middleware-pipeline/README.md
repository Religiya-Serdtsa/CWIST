# Tutorial 04: Middleware Pipeline Registration

Attach global middleware handlers to inspect, log, or transform incoming HTTP requests.

## Key Concepts
- Register global middleware using `cwist_app_use(app, middleware_fn)`.
- Middleware functions execute prior to route-specific handler invocation.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut04
```

Test with `curl`:
```bash
curl http://127.0.0.1:8083/dashboard
```
