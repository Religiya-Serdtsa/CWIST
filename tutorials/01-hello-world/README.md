# Tutorial 01: Hello World HTTP Server

Learn how to set up a minimal CWIST HTTP web server and register a basic route handler.

## Key Concepts
1. `cwist_app_create()`: Allocates and initializes a new CWIST application instance.
2. `cwist_app_get(app, path, handler)`: Registers a GET route handler callback.
3. `cwist_app_listen(app, port)`: Binds to the target port and starts the non-blocking event loop.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut01
```

Test with `curl`:
```bash
curl http://127.0.0.1:8080/
# Output: Hello, CWIST Web Framework!
```
