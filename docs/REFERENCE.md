# CWIST Library Reference

CWIST is a custom C library for building secure, scalable web applications. It includes modules for string handling, HTTP/HTTPS, URI parsing, database access, and cryptographic hashing.

## Modules

### 1. SString (Smart String)
A safe, dynamic string implementation.
- **Header:** `<cwist/core/sstring/sstring.h>`
- **Features:** Automatic resizing, concatenation, safe manipulation.

### 2. HTTP Server
A multi-process/threaded HTTP server framework.
- **Header:** `<cwist/net/http/http.h>`
- **Structures:** `cwist_http_request`, `cwist_http_response`
- **Features:** 
  - Request parsing
  - Response serialization
  - Helper functions for headers/status codes.

### 3. HTTPS Support
Secure transport layer using OpenSSL.
- **Header:** `<cwist/net/http/https.h>`
- **Features:**
  - `cwist_https_init_context`: Loads Cert/Key.
  - `cwist_https_accept`: SSL Handshake.
  - `cwist_https_send_response`: Encrypted sending.
  - **Note:** Internally reuses `cwist/http.h` for logic, adding a security layer.

### 4. Query Parsing
Robust query string parsing using `liburiparser`.
- **Header:** `<cwist/net/http/query.h>`
- **Function:** `void cwist_query_map_parse(map, raw_query)`
- **Behavior:** Parses `key=value&key2=val2` into a hash map (SipHash).

### 5. Database (SQL)
A wrapper around `sqlite3` for persistent storage.
- **Header:** `<cwist/core/db/sql.h>`
- **Features:**
  - `cwist_db_open`: Connect to DB file.
  - `cwist_db_exec`: Run commands (CREATE/INSERT/UPDATE).
  - `cwist_db_query`: Run SELECT and get results as `cJSON` array.

### 6. SipHash
Cryptographic hash function for hash maps (used in Query/Headers).
- **Header:** `<cwist/core/siphash/siphash.h>`

### 7. Error Handling
Unified error handling system using `cwist_error_t`.
- **Header:** `<cwist/sys/err/cwist_err.h>`
- **Features:** Can return simple integer codes or complex JSON objects (e.g., OpenSSL/SQLite errors).

### 8. Big Dumb Reply (BDR)
Auto-caches serialized responses for expensive handlers.
- **Header:** `<cwist/sys/app/big_dumb_reply.h>`
- **Functions:** `cwist_bdr_get`, `cwist_bdr_put`, `cwist_bdr_set_limits`.
- **Guard Rails:** Entries expire after a configurable TTL or hit budget and the cache maintains a soft byte cap (32 MiB by default). Use `cwist_app_configure_bdr` to tune per-application behavior.

## LibTTAK Memory Features

CWIST embeds libttak and exposes its latest memory runtime:

- **Generational Arenas:** Static assets and cache entries are tracked via `ttak_mem_tree` and retired generation-by-generation to avoid RSS spikes.
- **Epoch-Based Reclamation:** Hot reloads and Big Dumb Reply swaps call `ttak_epoch_enter/exit` so readers never block when arenas rotate.
- **Detachable Memory:** `ttak_detachable_mem_alloc` backs zero-copy HTTP bodies; cached slices reuse the tiny detachable cache and register cleanup hooks with `cwist_http_response_set_body_ptr_managed`.

See `example/rps-showcase/` for a runnable demonstration that combines all three pieces to keep `/rps` saturated during load tests.

## Example: Othello Web
Located in `example/othello-web/`.
- **Stack:** HTML/CSS/JS (Frontend), C (Backend).
- **Features:**
  - HTTPS (port 8443).
  - Multiplayer (SQLite backed).
  - Rooms support (via `?room=ID`).
  - Strategic Hints (Frontend JS).

## Dependencies
- `libssl-dev` (OpenSSL)
- `libcjson-dev` (JSON)
- `liburiparser-dev` (URI Parsing)
- `libsqlite3-dev` (Database)

## Build
Run `make` to build the static library `libcwist.a`.
Run `make test` to run unit tests.
