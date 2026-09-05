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

## Section Progress Summary

```
[P0: Critical]      ████████████████████ 100% (Completed)
[P1: Production]     ████████████████████ 100% (Multiport hardening complete)
[P2: DevEx]          ████████████████████ 100% (30 English hands-on tutorials, CLI, hot reload, and test client complete)
[P3: Deep Protocols] ███████████████░░░░░  75% (HTTP/3 extension specs and io_uring backend done)
[P4: Ecosystem]      ███████████████░░░░░  75% (gRPC wire streaming, trailers, deadlines, gzip, and proto codegen done)
```

### 1) Transport Layer

* **Native protocols ready**: HTTP/1.1 through HTTP/3 (QUIC via `lsquic`), WebSocket, SSE, and a bounded GraphQL query layer are implemented in-tree (WebTransport server/client evaluation is isolated to `dev`). Low-level socket controls (ECN, 0-RTT, connection migration) are complete.
* **HTTP/3 browser hardening**: Response header emission now normalizes field names to lowercase and rejects CR/LF-bearing values, covering login/logout cookie and redirect paths in strict browsers such as Firefox.
* **HTTPS hot-path optimization**: HTTP/1.1 connections now remain alive across requests, TLS writes stream headers and bodies separately without an intermediate response blob, and prefork workers share session-ticket keys for cross-worker resumption.
* **Request-memory optimization**: Parsed request strings and static response headers use arena-backed or borrowed storage with copy-on-write detachment, while received bodies can transfer ownership without a second copy.
* **Async I/O optimization (`io_uring`)**: io_uring serves as the readiness/wait layer inside `reactor.c` (epoll_wait replacement); its ring setup, teardown, and free-stack slot infrastructure were absorbed from the retired completion-based backend.
* **Multiport HTTP/3 fan-out**: The `cwist_multiport_t` facade now creates per-port UDP contexts and copies global HTTP/3 settings unless a port is detached into a sub-app.

### 2) Application Layer

* **High-performance router & middleware**: Deterministic resource management with parameterized routes (`/user/:id`), compression (Gzip via zlib), CORS, and rate limiting (libttak token bucket) are integrated.
* **Observability**: Prometheus `/metrics` and a probe-registry health-check system are operational.
* **Per-port sub-applications**: Lifecycle and exception handling for `cwist_multiport_get_app(&app, port)` are hardened; detached ports are separately tunable sub-applications.
* **gRPC services**: Applications can register unary and streaming handlers, incrementally decode arbitrarily split gRPC frames, attach transport output sinks, expose standard health/reflection services, and generate C models/method paths with `cwist proto`.
* **Packaging**: `libcwist.a` is a CWIST-only static archive; bundled dependency archives and public headers install side-by-side with `PREFIX`/`DESTDIR` staging support. `cwist.pc` pkg-config metadata, a versioned dist tarball (`dist/cwist-3.2.tar.gz`), and Homebrew (`packaging/homebrew/cwist.rb`) / vcpkg (`packaging/vcpkg/`) packaging drafts are available.
* **Deferred async handlers**: `cwist_async_defer()` hands a request/response pair to any thread (scheduler job, NATS callback, custom worker) for later completion via `cwist_async_respond()` / `respond_with()` / `abort()`, with optional 504 timeout and per-mode completion routing (reactor-posted in C1M, inline in thread-pool mode).

### 3) Security & Data Layer

* **Security specs**: BoringSSL-based TLS 1.3 and hybrid post-quantum KEM (`X25519MLKEM768`) are implemented ahead of time. CSRF uses a 256-bit double-submit token with constant-time validation; WAF-lite uses bounded linear scans and HTML output escaping.
* **Data-layer integrity**: SQLite3 embedded integration, migration system, and a `_Generic` macro-based type-dispatched ORM/query builder are in the build stream. The lock-free work queue (`cwist_io_queue`) protects node reclamation with ttak EBR critical sections and a two-stage retire deferral across global-epoch boundaries, and scheduler-backed background jobs are implemented.
* **Protobuf wire helpers**: A lightweight Protobuf runtime supports varint keys, unsigned/signed/bool fields, length-delimited bytes/strings, reader iteration, and ZigZag helpers for hand-written services.

---

## Current Snapshot

<!-- CI-BENCHMARKS:START -->
Automated OS benchmark history is published in `docs/benchmark-trends.svg`. Latest platform: **Darwin**.
<!-- CI-BENCHMARKS:END -->

- Core HTTP/1.1, HTTP/2, HTTP/3, WebSocket, routing, middleware, validation, metrics, health checks, static-file caching, and graceful shutdown are fully implemented in-tree.
- **Developer Ecosystem & Tutorials**: 30 comprehensive hands-on tutorial modules with C source (`main.c`), `CMakeLists.txt`, and English documentation guides (`README.md`) are available under `tutorials/`.
- **CI Automated Web Server Benchmark**: Inline CI job dynamically generates and measures CWIST, Axum, and Spring Boot web servers using `wrk`, rendering real-time RPS, Latency, Peak RSS, and Context Switch metrics. A `the-benchmarker/web-frameworks` contract app lives in `benchmarks/web-frameworks/`, pinned to the `v3.2` tag with the uriparser lib path wired in.
- **P0 (must-have) is 100% complete**: the framework’s core architecture and protocol stack are locked.
- We are now in the **P2–P4 tooling and ecosystem phase**. Completed multiport, scheduler, test-client, `io_uring`, deferred async handlers, end-to-end streaming gRPC (DATA-frame wiring, trailers, deadlines, gzip), and Protobuf wire-format work remain covered by focused regression tests.

---

## 1. Transport Layer

| Feature | Status | Notes |
|---------|--------|-------|
| HTTP/1.1 Server (epoll, threading, forking) | ✅ | Zero-copy sendfile, keep-alive, and supervisor-to-worker shutdown propagation |
| HTTP/2 Server | ✅ | h2 with ALPN |
| HTTP/3 Server (QUIC) | ✅ | lsquic + BoringSSL, QPACK, 0-RTT, migration, push, resilience timeout knobs, lowercase/CRLF-safe response headers |
| HTTP/1.1 + HTTP/2 Client | ✅ | libcurl based, sync & async APIs |
| HTTP/3 Client | ✅ | lsquic based, async stream callbacks, auto-retry with exponential backoff, conn timeout knobs |
| WebSocket Server | ✅ | Upgrade, frame parsing, ping/pong |
| TLS 1.3 / HTTPS | ✅ | BoringSSL, ECH, persistent HTTP/1.1 requests, split header/body writes, and shared prefork session-ticket keys |
| Alt-Svc Header Injection | ✅ | HTTP/3 upgrade advertisement from HTTP/1.1/2 |
| **io_uring Backend** | ✅ | io_uring readiness multiplexer in `src/sys/io/reactor.c` (one-shot POLL_ADD wait layer, epoll fallback); request I/O hot path stays synchronous |
| **kqueue Backend** | ✅ | `src/sys/io/kqueue.c`, BSD/macOS I/O multiplexing event loop, integration tests in GitHub Actions CI gate |
| HTTP/2 Server Push | ✅ | `cwist_http2_push_resource` with PUSH_PROMISE frame, HPACK encoding, server-initiated even stream IDs |
| **WebTransport** | ⏳ / 🔮 | Excluded from `main`; experimental WebTransport server/native C client proposal (LSQUIC PR #629) is evaluated exclusively on `dev` |
| HTTP/3 Datagram Extension | ✅ | `send_datagram`, callbacks, `es_datagrams` enabled |
| ECN (Explicit Congestion Notification) | ✅ | UDP socket with `IP_RECVTOS` / `IPV6_RECVTCLASS` |
| Connection Migration | ✅ | `es_allow_migration` enabled |
| 0-RTT Early Data | ✅ | Client: `cwist_http3_client_enable_0rtt`; Server: opt-in via `cwist_http3_set_early_data` (default OFF), shared session-ticket keys for resumption, and a default-ON replay guard restricting early-data requests to idempotent methods |
| **Multiport TCP Facade** | ✅ | Counted `cwist_multiport_t` descriptor, shared accept loop, duplicate/default-port validation, and per-port smoke tests |
| **Multiport HTTP/3 Fan-out** | ✅ | One UDP socket/context per bound port, with global settings copied unless the port is detached into a sub-app |

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
| **SSE (Server-Sent Events)** | ✅ | Buffered and live structured events, IDs, retry directives, multiline data, comments, and convenience macros |
| **Access Logging** | ✅ | Common, Combined, and JSON formats implemented |
| **Request ID / Tracing** | ✅ | X-Request-Id middleware injects and propagates request IDs |
| Graceful Shutdown | ✅ | Unified atomic `running` flag + SIGTERM/SIGINT handlers across HTTP/1.1, HTTP/2, HTTP/3 loops |
| **Health Check Endpoint** | ✅ | `/healthz`, `/live`, `/ready` with probe registry and auto-registration |
| **Metrics / Observability** | ✅ | Prometheus `/metrics` endpoint wired; request counter & duration middleware |
| **Per-Status Error Handlers** | ✅ | `cwist_app_register_error_handler` for custom 404, 500, etc. |
| **URL Reverse Routing** | ✅ | `cwist_app_get_named` + `cwist_url_for` with param substitution |
| **Flash Messages** | ✅ | One-time session-scoped messages via `cwist_flash_get/set` |
| **Per-Port Sub-Applications** | ✅ | `cwist_multiport_get_app(&app, port)` detaches additional ports for independent tuning; public/default port remains owned by root app |
| **Standard Status Codes** | ✅ | Full 1xx–5xx `cwist_http_status_t` enum (RFC 9110 + WebDAV extensions), `cwist_http_status_reason()` table, and automatic reason phrases when handlers only set the numeric code |
| **Async / Deferred Handlers** | ✅ | `cwist_async_defer` parks the request for cross-thread completion (`respond` / `respond_with` / `abort`), optional 504 timeout, reactor-posted completion in C1M mode with keep-alive re-arm; covered by `test_async_defer` |

---

## 3. Security

| Feature | Status | Notes |
|---------|--------|-------|
| JWT (encode/decode/verify) | ✅ | HS256 / RS256 |
| Database Encryption | ✅ | `db_crypt` layer |
| ECH (Encrypted Client Hello) | ✅ | BoringSSL ECH |
| **PQC Hybrid KEM (TLS)** | ✅ | `cwist_app_use_pqc_layer` forces `X25519MLKEM768:X25519:P-256`, TLS 1.3 only |
| **CSRF Protection** | ✅ | 256-bit double-submit cookie, constant-time comparison, strict SameSite, header and URL-encoded form support |
| **Secure Headers** | ✅ | Automatic injection of HSTS, CSP, X-Frame-Options, Referrer-Policy, CORP via `cwist_http_response_add_security_headers()` |
| **Request Size Limits** | ✅ | HTTP/1.1/2/3 body limits audited and enforced (`CWIST_HTTP_MAX_BODY_SIZE`) |
| **Input Validation** | ✅ | Bind validator added (`bind.c`, `bind.h`, `test_bind.c`) |
| **WAF-lite / Sanitization** | ✅ | Linear-time request signature checks plus `cwist_sanitize_html()` output escaping; parameterized SQL remains required |

---

## 4. Data Layer

| Feature | Status | Notes |
|---------|--------|-------|
| SQLite Integration | ✅ | `sqlite3` embedded |
| Database Migration | ✅ | `migrate` system |
| **Connection Pool** | ✅ | Bounded SQLite pool with shared `:memory:` URI mode, O(1) leasing, timeout acquisition, and graceful drain on destroy |
| **ORM / Query Builder** | ✅ | Socket-backed ORM with dialect-aware query builder, `_Generic` type-dispatched RETURNING / scalar helpers |
| **Redis / Key-Value Cache** | ✅ | RESP2 client/pool, binary-safe argv commands, AUTH/SELECT, pub/sub, and app-level pool integration |
| NATS Integration | ✅ | `cwist_nats` wrapper |
| **Message Queue (Job Queue)** | ✅ | `cwist_io_queue` lock-free job queue plus scheduler-backed immediate and delayed jobs |

---

## 5. Developer Experience

| Feature | Status | Notes |
|---------|--------|-------|
| Doxygen Docs | ✅ | Generated HTML docs |
| README / API Reference | ✅ | Markdown docs in `docs/` |
| **Tutorial & Examples** | ✅ | 30 comprehensive hands-on tutorial modules with C source, CMakeLists, and English guides in `tutorials/` |
| **CLI Scaffolding** | ✅ | `cwist new project`, `.cwpro` manifests, OpenAPI generation, and include-aware incremental watcher |
| **Hot Reload (Dev Mode)** | ✅ | `cwist watcher` uses inotify/kqueue with snapshot/poll fallback, debounces changes, exports include-graph scope, preserves prior process on build failure, and performs zero-downtime SO_REUSEPORT overlap process hot-swapping |
| **Configuration Management** | ✅ | `.env` file + environment variable loader via `cwist_config` |
| **Static Library Packaging** | ✅ | CWIST-only archive plus separately installed bundled libraries/headers, deterministic static-link order, `PREFIX`/`DESTDIR` staging, `cwist.pc` pkg-config file, versioned dist tarball, and Homebrew/vcpkg packaging drafts |
| **Testing Utilities** | ✅ | In-process test client (`cwist_test_client`), cookie jar, multipart helper, and BDD-style fluent assertions (`CWIST_ASSERT_STATUS`, `CWIST_ASSERT_HEADER`, `CWIST_ASSERT_BODY_CONTAINS`) |
| **Interactive API Documentation** | ✅ | Embedded Swagger UI interactive documentation page (`cwist_app_enable_swagger`) serving `/docs` and `/openapi.json` |
| **Benchmark Suite** | ✅ | GitHub Actions Linux/macOS measurements publish CPU, throughput, RSS, memory-recovery drift, and context-switch SVG trends; `benchmarks/web-frameworks/` contract app pinned to `v3.2` for the-benchmarker harness |
| **Fuzzing / Hardening** | ✅ | Stateful sequence/auth libFuzzer coverage plus bounded reassembly and strict HTTP chunk framing checks |

---

## 6. Ecosystem & Integrations

| Feature | Status | Notes |
|---------|--------|-------|
| **gRPC over HTTP/2** | ✅ | Unary/stream registration, split-frame incremental decoder and output sink, standard health/reflection service registration, gRPC metadata, and test-client coverage |
| **Protobuf Runtime Helpers** | ✅ | Wire-format reader/writer for varint, bool, bytes/string, signed integer casting, and ZigZag helpers |
| **GraphQL** | ✅ | Full query/mutation engine, field arguments, variables, aliases, nested selection sets, error envelope, and HTTP adapter |
| **OpenAPI / Swagger Generation** | ✅ | OpenAPI 3.1 JSON generated from Doxygen `@openapi.*` annotations on route declarations |
| **Background Jobs / Scheduler** | ✅ | `cwist_scheduler` worker pool with immediate and delayed job execution |
| **WebRTC** | 🔮 | Real-time media; requires separate data channel stack |
| **Serverless / WASM Runtime** | 🔮 | Edge deployment target |

---

## Current Focus (P2 – P4 Tooling and Ecosystem)

The completed P1-P3 hardening work is now under regression coverage. Current priorities are developer workflow and ecosystem integrations.

### Developer Workflow

* Expand benchmark automation (the-benchmarker contract app) and fuzz targets.
* Add a POLLOUT-resumable writer for deferred async completions so slow clients cannot occupy a reactor thread for the SO_SNDTIMEO budget.
* Keep test-client, scheduler, multiport, `io_uring`, and deferred-async coverage in the default test harness.

### Ecosystem

* Extend `cwist proto` beyond scalar proto3 models to repeated, nested, enum, and descriptor-set bindings.
* Add gRPC deadlines, cancellation propagation, metadata normalization, retry policy, and load balancing.
* Extend the GraphQL subset with schema validation, mutations, nested selections, and subscriptions.
* Stabilize the experimental native C WebTransport client after LSQUIC PR #629 merges upstream.
* Evaluate persistent job backends separately from the in-process queue/scheduler.

### gRPC / Protobuf Status

Completed:

* `cwist_app_grpc_unary(app, service, method, handler, user_ctx)` registers `POST /Service/Method` routes for HTTP/2 gRPC unary calls.
* `cwist_app_grpc_stream(app, service, method, handler, user_ctx)` registers buffered streaming handlers that can consume multiple request messages and append multiple response messages in order.
* `cwist_grpc_decode_message()` and `cwist_grpc_encode_message()` implement the gRPC 5-byte message envelope (`compressed` flag + big-endian payload length).
* `cwist_grpc_decode_next_message()` iterates concatenated gRPC message envelopes for client-streaming and bidi-style buffered request bodies.
* `cwist_grpc_stream_send()` and `cwist_grpc_stream_close()` build ordered multi-message gRPC responses with final status metadata.
* `cwist_grpc_decoder_feed()` recovers gRPC envelopes split across arbitrary DATA payload boundaries; `cwist_grpc_stream_set_writer()` permits immediate transport-frame output.
* `cwist_app_grpc_health()` and `cwist_app_grpc_health_set_status()` register and control `grpc.health.v1.Health`; `cwist_app_grpc_reflection()` registers the v1alpha reflection stream.
* `cwist proto input.proto` generates scalar proto3 C models, encoder helpers, and gRPC method-path constants.
* `cwist_grpc_set_response()` and `cwist_grpc_set_error()` produce `application/grpc` responses and explicit `grpc-status` / `grpc-message` metadata.
* `cwist_pb_writer` supports varint keys, uint64/int64/bool fields, bytes fields, string fields, and dynamic buffer growth.
* `cwist_pb_reader` iterates Protobuf fields and exposes wire type, field number, varint value, and length-delimited payload slices.
* `cwist_pb_zigzag_encode()` / `cwist_pb_zigzag_decode()` cover signed integer mappings used by `sint32` / `sint64` style fields.
* `test_grpc` verifies Protobuf request construction, gRPC frame handling, unary dispatch, buffered streaming dispatch, multi-message response parsing, invalid content type handling, and malformed frame rejection.

* HTTP/2 streaming calls are wired end-to-end: inbound DATA frames feed `cwist_grpc_decoder_feed()` directly (client-streaming handlers see messages as they arrive via the blocking `cwist_grpc_stream_recv()`), and `cwist_grpc_stream_send()` flushes DATA frames immediately from the handler thread. The app server wires this via `cwist_http2_serve_connection_ex()` + `cwist_grpc_http2_hooks()`.
* `grpc-status` / `grpc-message` are emitted in a dedicated HTTP/2 trailer HEADERS frame (END_STREAM) for both unary and streaming responses.
* `grpc-timeout` is parsed (`cwist_grpc_parse_timeout()`), enforced on streaming calls (DEADLINE_EXCEEDED trailers plus handler cancellation), and client RST_STREAM propagates to handlers via `cwist_grpc_stream_cancelled()` / a `-1` recv.
* Request metadata is normalized: header names are lowercased at the HTTP/2 layer, lookups are case-insensitive (`cwist_grpc_metadata_get()`), and `*-bin` values are base64-decoded (`cwist_grpc_metadata_get_binary()`).
* Compression negotiation: `grpc-encoding: gzip` request messages are inflated (zlib), `grpc-accept-encoding: gzip, identity` is advertised, and unsupported encodings are rejected with `UNIMPLEMENTED`.
* `test_grpc` covers the buffered path (unary/streaming dispatch, metadata, gzip, recv replay, timeout parsing); `test_grpc_stream` covers the wire path over h2c (split DATA delivery, trailer frames, deadline, RST cancellation, gzip, unsupported-encoding rejection).

Known limits:

* Server-side response compression is not implemented (requests only); handler-thread sends do not block waiting for WINDOW_UPDATE (a zero-credit send fails with UNAVAILABLE).
* The proto generator currently supports scalar proto3 model encoders and service paths, not repeated/nested/enum/descriptor-set bindings.
* The builtin health `Watch` route stays on the buffered dispatch path.
* No gRPC client, retry policy, or load-balancing policy exists yet.

---

## Priority Queue (Suggested)

### P0 — Framework Gap (Must Have) ✅ COMPLETE
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
11. ~~**Secure Headers** (HSTS, CSP, X-Frame-Options, etc.)~~ ✅
12. ~~**Request Size Limits** (HTTP/1.1/2/3 body limit audit)~~ ✅
13. ~~**Multiport facade hardening**: counted port descriptor, per-port sub-app lifecycle, duplicate/default-port validation, and smoke tests~~ ✅

### P2 — Developer Velocity
14. ~~**Hot Reload** for development~~ ✅
15. ~~**CLI Tooling** (project scaffold, route generator, watcher)~~ ✅
16. ~~**Configuration** loader (`.env`, `.toml`)~~ ✅
17. ~~**Test Harness** with HTTP mock client~~ ✅
18. ~~**Deferred Async Handlers** (`cwist_async_defer` cross-thread completion)~~ ✅

### P3 — Advanced Protocols
19. **Native C WebTransport client stabilization** (experimental `dev` implementation available)
20. ~~**HTTP/2 Server Push**~~ ✅
21. ~~**io_uring** UDP packet loop for HTTP/3~~ ✅
22. ~~**kqueue** backend for macOS/BSD~~ ✅
23. ~~**Multiport HTTP/3 parity**: per-port UDP contexts and global setting propagation to non-detached ports~~ ✅

### P4 — Ecosystem
24. ~~**gRPC unary and buffered streaming server support**~~ ✅
25. ~~**GraphQL** bounded Query executor~~ ✅
26. ~~**OpenAPI** generator~~ ✅
27. ~~**Background Jobs / Scheduler**~~ ✅
28. ~~**Incremental gRPC framing, reflection, health checks, and `.proto` codegen**~~ ✅
29. ~~**gRPC wire streaming**: DATA-frame wiring, trailers, deadlines, gzip negotiation~~ ✅
30. **Distribution**: publish Homebrew formula and vcpkg port beyond the current drafts

---

## Contributing

If you want to pick up an item, open an issue referencing this roadmap and the specific feature number.
