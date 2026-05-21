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

## Current Snapshot

- Core HTTP/1.1, HTTP/2, HTTP/3, WebSocket, routing, middleware, validation, metrics, health checks, static-file caching, and graceful shutdown are already implemented in-tree.
- The application layer is being extended with a multiport facade: `cwist_multiport_t` converts normal C port arrays into counted descriptors, and `cwist_multiport_get_app(&app, port)` is intended to detach an additional port into a tunable sub-application.
- Current multiport work builds as part of `libcwist.a`, but still needs focused tests, API docs, and protocol parity hardening before it should be marked stable.

---

## 1. Transport Layer

| Feature | Status | Notes |
|---------|--------|-------|
| HTTP/1.1 Server (epoll, threading, forking) | ✅ | Zero-copy sendfile, keep-alive |
| HTTP/2 Server | ✅ | h2 with ALPN |
| HTTP/3 Server (QUIC) | ✅ | lsquic + BoringSSL, QPACK, 0-RTT, migration, push, resilience timeout knobs |
| HTTP/1.1 + HTTP/2 Client | ✅ | libcurl based, sync & async APIs |
| HTTP/3 Client | ✅ | lsquic based, async stream callbacks, auto-retry with exponential backoff, conn timeout knobs |
| WebSocket Server | ✅ | Upgrade, frame parsing, ping/pong |
| TLS 1.3 / HTTPS | ✅ | BoringSSL, ECH support |
| Alt-Svc Header Injection | ✅ | HTTP/3 upgrade advertisement from HTTP/1.1/2 |
| **io_uring Backend** | 🔄 | Initial implementation added (`io_uring_backend.c`, `test_io_uring.c`) |
| **kqueue Backend** | ⏳ | BSD/macOS; blocked on non-Linux test environment |
| HTTP/2 Server Push | ✅ | `cwist_http2_push_resource` with PUSH_PROMISE frame, HPACK encoding, server-initiated even stream IDs |
| **WebTransport** | ✅ | Basic server handler (`:protocol=webtransport` detection via HTTP/3 CONNECT) |
| HTTP/3 Datagram Extension | ✅ | `send_datagram`, callbacks, `es_datagrams` enabled |
| ECN (Explicit Congestion Notification) | ✅ | UDP socket with `IP_RECVTOS` / `IPV6_RECVTCLASS` |
| Connection Migration | ✅ | `es_allow_migration` enabled |
| 0-RTT Early Data | ✅ | `SSL_CTX_set_early_data_enabled` |
| **Multiport TCP Facade** | 🔄 | Counted `cwist_multiport_t` descriptor and shared accept loop are in progress; per-port tests and docs pending |
| **Multiport HTTP/3 Fan-out** | ⏳ | Needs one UDP socket/context per bound port, with global settings copied unless the port is detached into a sub-app |

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
| **Compression (gzip / brotli)** | ✅ | Compression middleware with swappable backend (`gzip` via zlib), `cwist_mw_compress` factory, tests added |
| **Caching Layer** | ✅ | ETag, Last-Modified, Cache-Control, 304 Not Modified for static files |
| **Rate Limiting** | ✅ | Per-IP token bucket via libttak; parameter respected |
| **CORS** | ✅ | Permissive CORS + preflight handler implemented |
| **SSE (Server-Sent Events)** | ⏳ | No structured SSE stream API |
| **Access Logging** | ✅ | Common, Combined, and JSON formats implemented |
| **Request ID / Tracing** | ✅ | X-Request-Id middleware injects and propagates request IDs |
| Graceful Shutdown | ✅ | Unified atomic `running` flag + SIGTERM/SIGINT handlers across HTTP/1.1, HTTP/2, HTTP/3 loops |
| **Health Check Endpoint** | ✅ | `/healthz`, `/live`, `/ready` with probe registry and auto-registration |
| **Metrics / Observability** | ✅ | Prometheus `/metrics` endpoint wired; request counter & duration middleware |
| **Per-Status Error Handlers** | ✅ | `cwist_app_register_error_handler` for custom 404, 500, etc. |
| **URL Reverse Routing** | ✅ | `cwist_app_get_named` + `cwist_url_for` with param substitution |
| **Flash Messages** | ✅ | One-time session-scoped messages via `cwist_flash_get/set` |
| **Per-Port Sub-Applications** | 🔄 | `cwist_multiport_get_app(&app, port)` detaches additional ports for independent tuning; public/default port must remain owned by root app |

---

## 3. Security

| Feature | Status | Notes |
|---------|--------|-------|
| JWT (encode/decode/verify) | ✅ | HS256 / RS256 |
| Database Encryption | ✅ | `db_crypt` layer |
| ECH (Encrypted Client Hello) | ✅ | BoringSSL ECH |
| **PQC Hybrid KEM (TLS)** | ✅ | `cwist_app_use_pqc_layer` forces `X25519MLKEM768:X25519:P-256`, TLS 1.3 only |
| **CSRF Protection** | ⏳ | No double-submit cookie or synchronizer token |
| **Secure Headers** | ⏳ | No automatic HSTS, CSP, X-Frame-Options injection |
| **Request Size Limits** | 🔄 | HTTP/3 has body limit; HTTP/1.1/2 limits need audit |
| **Input Validation** | ✅ | Bind validator added (`bind.c`, `bind.h`, `test_bind.c`) |
| **WAF-lite / Sanitization** | ⏳ | No XSS/SQLi sanitizer middleware |

---

## 4. Data Layer

| Feature | Status | Notes |
|---------|--------|-------|
| SQLite Integration | ✅ | `sqlite3` embedded |
| Database Migration | ✅ | `migrate` system |
| **Connection Pool** | ⏳ | SQLite is direct; no generic connection pool abstraction |
| **ORM / Query Builder** | ✅ | Socket-backed ORM with dialect-aware query builder, _Generic type-dispatched RETURNING / scalar helpers |
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
3. ~~**Graceful Shutdown** unified across HTTP/1.1, HTTP/2, HTTP/3~~ ✅
4. ~~**Compression** (gzip at minimum, brotli preferred)~~ ✅
5. ~~**Form / Request Validation** middleware~~ ✅

### P1 — Production Readiness
6. ~~**Access Logs** (Common/JSON format)~~ ✅
7. ~~**Metrics endpoint** (Prometheus text format)~~ ✅
8. ~~**Rate Limiting** middleware~~ ✅
9. ~~**Caching** (ETag generation + in-memory cache)~~ ✅
10. ~~**Health Check** endpoints~~ ✅
11. **Multiport facade hardening** 🔄: counted port descriptor, per-port sub-app lifecycle, duplicate/default-port validation, and smoke tests

### P2 — Developer Velocity
12. **Hot Reload** for development
13. **CLI Tooling** (project scaffold, route generator)
14. ~~**Configuration** loader (`.env`, `.toml`)~~ ✅
15. **Test Harness** with HTTP mock client 🔄

### P3 — Advanced Protocols
16. ~~**WebTransport** server + client~~ ✅ (basic server handler)
17. ~~**HTTP/2 Server Push**~~ ✅
18. **io_uring** UDP packet loop for HTTP/3 🔄
19. **kqueue** backend for macOS/BSD
20. **Multiport HTTP/3 parity** ⏳: per-port UDP contexts and global setting propagation to non-detached ports

### P4 — Ecosystem
21. **gRPC** support
22. **GraphQL** executor
23. **OpenAPI** generator

---

## Contributing

If you want to pick up an item, open an issue referencing this roadmap and the specific feature number. Items marked 🔄 have partial code in-tree and may only need polish or integration.
