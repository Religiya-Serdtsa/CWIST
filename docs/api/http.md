# HTTP Core API

*Header:* `<cwist/net/http/http.h>`

Low-level HTTP structures, framing limits, and parsing logic.

## Constants

Defined in `<cwist/net/http/http.h>` to guard every connection against resource exhaustion.

- `CWIST_HTTP_MAX_HEADER_SIZE` – cap on accumulated header bytes (default 8KiB).
- `CWIST_HTTP_MAX_BODY_SIZE` – cap on POST body bytes kept in memory (default 10MiB).
- `CWIST_HTTP_READ_BUFFER_SIZE` – scratch buffer per connection for pipelined/keep‑alive traffic.
- `CWIST_HTTP_TIMEOUT_MS` – poll timeout used while waiting for header/body as well as while sending responses.

### `cwist_http_request_create`
```c
cwist_http_request *cwist_http_request_create(void);
```
Allocates a new HTTP request structure.
`cwist_app_listen` wires each inbound request with framework context:

- `req->app` – back-reference to the running `cwist_app` (useful for pulling global config).
- `req->db` – shared `cwist_db` handle configured via `cwist_app_use_db`.

### `cwist_http_parse_request`
```c
cwist_http_request *cwist_http_parse_request(const char *raw_request);
```
Parses a raw HTTP string into a `cwist_http_request` object.

### `cwist_http_receive_request`
```c
cwist_http_request *cwist_http_receive_request(
    int client_fd,
    char *read_buf,
    size_t buf_size,
    size_t *buf_len
);
```
Blocking, framed read helper for keep-alive sockets.
- Reads until `\r\n\r\n`, enforcing `CWIST_HTTP_MAX_HEADER_SIZE`.
- Parses headers plus `Content-Length` and continues polling/`recv`ing until the full body is buffered.
- Leaves any bytes for the *next* request in `read_buf`; caller passes the same buffer back in the next iteration.
- Returns `NULL` on timeout, malformed framing, or when limits are exceeded.

### `cwist_http_response_create`
```c
cwist_http_response *cwist_http_response_create(void);
```
Allocates a new HTTP response structure. Default status is 200 OK.

### `cwist_http_response_send_file`
```c
cwist_error_t cwist_http_response_send_file(
    cwist_http_response *res,
    const char *file_path,
    const char *content_type_hint,
    size_t *out_size
);
```
Reads a static file from disk, copies it into the response body, and sets a sensible `Content-Type`. Files larger than `CWIST_HTTP_MAX_BODY_SIZE` are rejected with `-EFBIG`. Returns `0` on success and propagates `-errno` on failures (`-ENOENT`, `-EISDIR`, etc.). `out_size` is optional; when provided it receives the file length so handlers can emit HEAD responses without buffering the payload.

### `cwist_http_header_add`
```c
cwist_error_t cwist_http_header_add(cwist_http_header_node **head, const char *key, const char *value);
```
Adds a header to the linked list.

### `cwist_http_header_get`
```c
char *cwist_http_header_get(cwist_http_header_node *head, const char *key);
```
Retrieves the value of a header by key. Returns `NULL` if not found.

## Server Core

### Keep-alive handling

`static_http_handler` in `src/framework/app.c` now loops on `cwist_http_receive_request` until either side requests `Connection: close`. Each response generated via `cwist_http_send_response()` contains an explicit `Content-Length`, and the send path uses `poll()`+`send(MSG_NOSIGNAL)` to avoid blocking indefinitely or tripping SIGPIPE while ApacheBench is bursting 64 concurrent keep-alive posts.

### Regression testing

`tests/stress_test.c` spins up a minimal `/api` endpoint and shells out to:

```
ab -k -n 20000 -c 64 -p payload.json -T application/json http://127.0.0.1:31744/api
```

The test succeeds only when the server drains each request body, recycles the leftover bytes for the next request on the same socket, and finalizes every response without triggering `apr_pollset_poll timeout` or `connection reset`.

### `cwist_http_server_loop`
```c
cwist_error_t cwist_http_server_loop(int server_fd, cwist_server_config *config, void (*handler)(int, void *), void *ctx);
```
Starts the main server loop. 
- Supports iterative, forking, and multithreaded models via `config`.
- `ctx` is passed to the handler for thread-safe state management.

### `cwist_accept_socket`
```c
cwist_error_t cwist_accept_socket(int server_fd, struct sockaddr *sockv4, void (*handler_func)(int, void *), void *ctx);
```
Low-level accept loop wrapper.
