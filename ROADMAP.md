# CWIST Roadmap

> CWIST is a high-performance C web framework supporting HTTP/1.1, HTTP/2, HTTP/3, and WebSocket. This document tracks what exists, what is actively being built, and what is still missing for a competitive modern web framework.

---

## Legend

| Symbol | Meaning |
|--------|---------|
| ✅ | Done |
| 🔄 | In Progress |
| ⏳ | Planned / Not Started |
| 🔮 | Research / Future |

---

## 1. Transport Layer

| Feature | Status | Notes |
|---------|--------|-------|
| HTTP/1.1 Server (epoll, threading, forking) | ✅ | Zero-copy sendfile, keep-alive |
| HTTP/2 Server | ✅ | h2 with ALPN |
| HTTP/3 Server (QUIC) | ✅ | lsquic + BoringSSL, QPACK, 0-RTT, migration, push |
| HTTP/1.1 + HTTP/2 Client | ✅ | libcurl based, sync & async APIs |
| HTTP/3 Client | ✅ | lsquic based, async stream callbacks |
| WebSocket Server | ✅ | Upgrade, frame parsing, ping/pong |
| TLS 1.3 / HTTPS | ✅ | BoringSSL, ECH support |
| Alt-Svc Header Injection | ✅ | HTTP/3 upgrade advertisement from HTTP/1.1/2 |
| **io_uring Backend** | ⏳ | Linux-only; `epoll` done, io_uring needs `liburing` or raw syscalls |
| **kqueue Backend** | ⏳ | BSD/macOS; blocked on non-Linux test environment |
| HTTP/2 Server Push | ⏳ | Only HTTP/3 push is implemented |
| **WebTransport** | ✅ | Basic server handler (`:protocol=webtransport` detection via HTTP/3 CONNECT) |
| HTTP/3 Datagram Extension | ✅ | `send_datagram`, callbacks, `es_datagrams` enabled |
| ECN (Explicit Congestion Notification) | ✅ | UDP socket with `IP_RECVTOS` / `IPV6_RECVTCLASS` |
| Connection Migration | ✅ | `es_allow_migration` enabled |
| 0-RTT Early Data | ✅ | `SSL_CTX_set_early_data_enabled` |

---

## 2. Application Layer

| Feature | Status | Notes |
|---------|--------|-------|
| Basic Router / Mux | ✅ | Path-based dispatch |
| **Advanced Router** | ✅ | Parameterized routes (`/user/:id`), route groups, per-route middleware, wildcards |
| **Middleware Pipeline** | ✅ | Global + per-route middleware chains supported |
| Static File Serving | ✅ | Fast path via `sendfile`, zero-copy `ptr_body` |
| Template Engine | ✅ | Custom template syntax |
| JSON Builder / Healer | ✅ | `json_builder`, `json_heal` |
| HTML / CSS Builder | ✅ | Programmatic HTML/CSS generation |
| **Form / Multipart Parser** | ✅ | `multipart/form-data` via `multipart-parser-c` submodule |
| **Compression (gzip / brotli)** | ⏳ | No built-in response compression |
| **Caching Layer** | ⏳ | No HTTP cache (ETag, Last-Modified, Cache-Control) or in-memory cache |
| **Rate Limiting** | ⏳ | Token bucket or leaky bucket not implemented |
| **CORS** | 🔄 | Basic CORS test exists; configurable CORS middleware missing |
| **SSE (Server-Sent Events)** | ⏳ | No structured SSE stream API |
| **Access Logging** | ⏳ | No standardized access log format (Common / Combined / JSON) |
| **Request ID / Tracing** | ⏳ | No distributed tracing or request correlation ID injection |
| Graceful Shutdown | 🔄 | HTTP/3 has `running` flag; HTTP/1.1/2 needs unified graceful stop |
| **Health Check Endpoint** | ⏳ | No built-in `/healthz` or readiness/liveness probe |
| **Metrics / Observability** | 🔄 | Basic structured logger exists; Prometheus endpoint missing |
| **Per-Status Error Handlers** | ✅ | `cwist_app_register_error_handler` for custom 404, 500, etc. |
| **URL Reverse Routing** | ✅ | `cwist_app_get_named` + `cwist_url_for` with param substitution |
| **Flash Messages** | ✅ | One-time session-scoped messages via `cwist_flash_get/set` |

---

## 3. Security

| Feature | Status | Notes |
|---------|--------|-------|
| JWT (encode/decode/verify) | ✅ | HS256 / RS256 |
| Database Encryption | ✅ | `db_crypt` layer |
| ECH (Encrypted Client Hello) | ✅ | BoringSSL ECH |
| **CSRF Protection** | ⏳ | No double-submit cookie or synchronizer token |
| **Secure Headers** | ⏳ | No automatic HSTS, CSP, X-Frame-Options injection |
| **Request Size Limits** | 🔄 | HTTP/3 has body limit; HTTP/1.1/2 limits need audit |
| **Input Validation** | 🔄 | `zod`-like validator exists but lacks comprehensive rule set |
| **WAF-lite / Sanitization** | ⏳ | No XSS/SQLi sanitizer middleware |

---

## 4. Data Layer

| Feature | Status | Notes |
|---------|--------|-------|
| SQLite Integration | ✅ | `sqlite3` embedded |
| Database Migration | ✅ | `migrate` system |
| **Connection Pool** | ⏳ | SQLite is direct; no generic connection pool abstraction |
| **ORM / Query Builder** | ⏳ | Raw SQL only; no ActiveRecord-style ORM |
| **Redis / Key-Value Cache** | ⏳ | No Redis client integration |
| NATS Integration | ✅ | `cwist_nats` wrapper |
| **Message Queue (Job Queue)** | 🔄 | `cwist_io_queue` is lock-free job queue; no persistent queue backend |

---

## 5. Developer Experience

| Feature | Status | Notes |
|---------|--------|-------|
| Doxygen Docs | ✅ | Generated HTML docs |
| README / API Reference | ✅ | Markdown docs in `docs/` |
| **Tutorial & Examples** | ⏳ | Few examples; no step-by-step tutorial |
| **CLI Scaffolding** | ⏳ | No `cwist new project` CLI tool |
| **Hot Reload (Dev Mode)** | ⏳ | No file watcher + auto-recompile |
| **Configuration Management** | ✅ | `.env` file + environment variable loader via `cwist_config` |
| **Testing Utilities** | 🔄 | In-process test client (`cwist_test_client_get/post`) added; harness WIP |
| **Benchmark Suite** | ⏳ | No `wrk`/`oha`/`h2load` benchmark automation |
| **Fuzzing / Hardening** | ⏳ | No AFL/libFuzzer targets for HTTP parser or QUIC path |

---

## 6. Ecosystem & Integrations

| Feature | Status | Notes |
|---------|--------|-------|
| **gRPC over HTTP/2** | ⏳ | No protobuf / gRPC server or client |
| **GraphQL** | ⏳ | No GraphQL parser or executor |
| **OpenAPI / Swagger Generation** | ⏳ | No automatic spec generation from route definitions |
| **Background Jobs / Scheduler** | ⏳ | No cron-like scheduler or deferred job system |
| **WebRTC** | 🔮 | Real-time media; requires separate data channel stack |
| **Serverless / WASM Runtime** | 🔮 | Edge deployment target |

---

## Priority Queue (Suggested)

### P0 — Framework Gap (Must Have)
1. ~~**Advanced Router** with parameterized routes and route groups~~ ✅
2. ~~**Multipart / File Upload** parser~~ ✅
3. **Graceful Shutdown** unified across HTTP/1.1, HTTP/2, HTTP/3
4. **Compression** (gzip at minimum, brotli preferred)
5. **Form / Request Validation** middleware

### P1 — Production Readiness
6. **Access Logs** (Common/JSON format)
7. **Metrics endpoint** (Prometheus text format)
8. **Rate Limiting** middleware
9. **Caching** (ETag generation + in-memory cache)
10. **Health Check** endpoints

### P2 — Developer Velocity
11. **Hot Reload** for development
12. **CLI Tooling** (project scaffold, route generator)
13. ~~**Configuration** loader (`.env`, `.toml`)~~ ✅
14. **Test Harness** with HTTP mock client 🔄

### P3 — Advanced Protocols
15. ~~**WebTransport** server + client~~ ✅ (basic server handler)
16. **HTTP/2 Server Push**
17. **io_uring** UDP packet loop for HTTP/3
18. **kqueue** backend for macOS/BSD

### P4 — Ecosystem
19. **gRPC** support
20. **GraphQL** executor
21. **OpenAPI** generator

---

## Contributing

If you want to pick up an item, open an issue referencing this roadmap and the specific feature number. Items marked 🔄 have partial code in-tree and may only need polish or integration.
