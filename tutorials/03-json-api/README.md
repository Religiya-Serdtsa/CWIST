# Tutorial 03: RESTful JSON API Server

Construct structured JSON responses using `cJSON` and custom response headers.

## Key Concepts
- Integrating `cJSON` for memory-safe JSON serialization.
- Setting explicit HTTP response headers via `cwist_http_header_add(&res->headers, "Content-Type", "application/json")`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut03
```

Test with `curl`:
```bash
curl -i http://127.0.0.1:8082/api/data
# Output: HTTP/1.1 200 OK
# Content-Type: application/json
# {"status":"success","code":200}
```
