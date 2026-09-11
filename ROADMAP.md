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
[P4: Ecosystem]      ████████████████░░░░  80% (gRPC client retry policy and load balancing done)
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
* **gRPC services**: Applications can register unary and streaming handlers, incrementally decode arbitrarily split gRPC frames, attach transport output sinks, expose standard health/reflection services, and generate C models/method paths with `cwist proto`. Clients use `cwist_grpc_channel`: dns/ipv4/ipv6 target resolution, `pick_first`/`round_robin` load balancing over per-address subchannels, and the gRFC A6 retry engine (exponential backoff, retryable codes, server pushback, throttling, transparent retries); error responses go out Trailers-Only so conforming clients can retry.
* **Packaging**: `libcwist.a` is a CWIST-only static archive; bundled dependency archives and public headers install side-by-side with `PREFIX`/`DESTDIR` staging support. `cwist.pc` pkg-config metadata, a versioned dist tarball (`dist/cwist-3.2.tar.gz`), and Homebrew (`packaging/homebrew/cwist.rb`) / vcpkg (`packaging/vcpkg/`) packaging drafts are available.
* **Deferred async handlers**: `cwist_async_defer()` hands a request/response pair to any thread (scheduler job, NATS callback, custom worker) for later completion via `cwist_async_respond()` / `respond_with()` / `abort()`, with optional 504 timeout and per-mode completion routing (reactor-posted in C1M, inline in thread-pool mode).

### 3) Security & Data Layer

* **Security specs**: BoringSSL-based TLS 1.3 and hybrid post-quantum KEM (`X25519MLKEM768`) are implemented ahead of time. CSRF uses a 256-bit double-submit token with constant-time validation; WAF-lite compiles its signature set into an Aho-Corasick automaton (single O(n) pass per input, ~10.7M checks/s benchmarked) plus HTML output escaping.
* **Data-layer integrity**: SQLite3 embedded integration, migration system, and a `_Generic` macro-based type-dispatched ORM/query builder are in the build stream. The lock-free work queue (`cwist_io_queue`) protects node reclamation with ttak EBR critical sections and a two-stage retire deferral across global-epoch boundaries, and scheduler-backed background jobs are implemented.
* **Protobuf wire helpers**: A lightweight Protobuf runtime supports varint keys, unsigned/signed/bool fields, length-delimited bytes/strings, reader iteration, and ZigZag helpers for hand-written services.

---

## Current Snapshot

<!-- CI-BENCHMARKS:START -->
Automated OS benchmark history is published in `docs/benchmark-trends.svg`. Latest platform: **Darwin**.
<!-- CI-BENCHMARKS:END -->

- Core HTTP/1.1, HTTP/2, HTTP/3, WebSocket, routing, middleware, validation, metrics, health checks, static-file caching, and graceful shutdown are fully implemented in-tree.
- **Developer Ecosystem & Tutorials**: 30 comprehensive hands-on tutorial modules with C source (`main.c`), `CMakeLists.txt`, and English documentation guides (`README.md`) are available under `tutorials/`.
- **CI Automated Web Server Benchmark**: Inline CI job dynamically generates and measures CWIST, Axum, and Spring Boot web servers using `wrk`, rendering real-time RPS, Latency, Peak RSS, and Context Switch metrics. A `the-benchmarker/web-frameworks` contract app lives in `benchmarks/web-frameworks/`, pinned to the `v3.3` tag with the uriparser lib path wired in.
- **P0 (must-have) is 100% complete**: the framework’s core architecture and protocol stack are locked.
- **Resolved**: HTTP/3 header-set objects for streams still open at engine destroy are now tracked per `cwist_http3_context` and swept on the engine/context teardown path; the `leak:cwist_h3_hsi_create` entry was removed from `tests/lsan.supp`.
- **Resolved**: Deferred async completions on the reactor path now park unsent bytes in an owned buffer and resume via one-shot POLLOUT (`cwist_reactor_add_out`), so slow clients no longer occupy a worker/reactor thread for the send budget; a deadline (keep-alive timeout, refreshed on progress) bounds parked writers.
- **Resolved**: gRPC handler-thread sends now wait for WINDOW_UPDATE credit via a condvar rendezvous signalled by the dispatcher (`h2_fc_wait_credit`), instead of failing a zero-credit send with UNAVAILABLE; RST/teardown/stall-timeout still fail fast.
- **Resolved (v3.4 perf wave)**: C1M reactor tail latency — request batches now yield cooperatively (`CWIST_HTTP_YIELD_BATCH`), batch responses coalesce into one writev per turn (256 KiB stash), reactor wake eventfds are registered with the ring (`IORING_REGISTER_EVENTFD`), and post bursts coalesce to a single wake. P99.999 CI measurement fixed end-to-end (lua output format + `$GITHUB_WORKSPACE` script path).
- **Resolved (v3.4 perf wave)**: BDR cache learn/read paths are lock-free (CAS-published entries, atomic blob swaps, EBR reclamation) and entries support hit-time revalidation hooks with zero-copy pointer swaps (`cwist_bdr_put_revalidatable`); classic pool gained `CWIST_POOL_PREWARM` / `CWIST_POOL_IDLE_TIMEOUT_MS` tunables; the HTTPS handshake shepherd shard count is tunable via `CWIST_HTTPS_HS_SHARDS`.
- **Resolved (v3.4 gRPC client wave)**: gRPC client retry policy and client-side load balancing are done — `cwist_grpc_channel` resolves dns/ipv4/ipv6 targets into per-address subchannels with connection backoff (doc/connection-backoff.md), balances calls with `pick_first`/`round_robin` (doc/load-balancing.md), and runs the gRFC A6 retry engine (jittered exponential backoff, retryable codes, server pushback, token-bucket throttling, transparent retries, commit-on-headers) configured via C structs or JSON service config. Error responses are Trailers-Only on both unary and streaming server paths so retries can actually happen; `test_grpc_channel` covers the matrix against loopback backends plus a raw GOAWAY fake.
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
| 0-RTT Early Data | ✅ | Server: opt-in via `cwist_http3_set_early_data` (default OFF), shared session-ticket keys for resumption, and a default-ON replay guard restricting early-data requests to idempotent methods |
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
| **Interop Gate** | ✅ | h2spec HTTP/2 conformance diff against a pinned baseline (`scripts/ci/h2spec_gate.sh`); new failures break the build. Builds run with `-Werror`, stack protector, `_FORTIFY_SOURCE=2`, PIE, and full RELRO on Linux |
| **Fuzzing / Hardening** | ✅ | Stateful sequence/auth libFuzzer coverage plus bounded reassembly and strict HTTP chunk framing checks |

---

## 6. Ecosystem & Integrations

| Feature | Status | Notes |
|---------|--------|-------|
| **gRPC over HTTP/2** | ✅ | Unary/stream registration, split-frame incremental decoder and output sink, standard health (streaming Watch)/reflection service registration, gRPC metadata, h2c/TLS client with unary and server-streaming calls, channel client with dns/ipv4/ipv6 resolution, `pick_first`/`round_robin` LB, and gRFC A6 retries, and test-client coverage |
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
* Keep test-client, scheduler, multiport, `io_uring`, and deferred-async coverage in the default test harness.

### Ecosystem

* Finish `cwist proto` for v3.4: ~~descriptor-set input~~ (`oneof`, `map`, fixed-width types, `double`, and `protoc --descriptor_set_out` input all done alongside scalar/enum/nested/repeated-packed).
* ~~Add gRPC client-side support: h2/h2c client, retry policy, and load balancing.~~ (h2c/TLS client with unary + server-streaming calls, deadlines, and cancellation; channel with `pick_first`/`round_robin` LB over per-address subchannels; gRFC A6 retry policy with backoff, pushback, throttling, and transparent retries — all shipped)
* Add gRPC server-side response compression.
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
* `cwist proto input.proto` (or a `protoc --descriptor_set_out` binary, auto-detected or via `--descriptor-set`) generates scalar proto3 C models, encoder helpers, and gRPC method-path constants.
* `cwist_grpc_set_response()` and `cwist_grpc_set_error()` produce `application/grpc` responses and explicit `grpc-status` / `grpc-message` metadata.
* `cwist_pb_writer` supports varint keys, uint64/int64/bool fields, bytes fields, string fields, and dynamic buffer growth.
* `cwist_pb_reader` iterates Protobuf fields and exposes wire type, field number, varint value, and length-delimited payload slices.
* `cwist_pb_zigzag_encode()` / `cwist_pb_zigzag_decode()` cover signed integer mappings used by `sint32` / `sint64` style fields.
* `test_grpc` verifies Protobuf request construction, gRPC frame handling, unary dispatch, buffered streaming dispatch, multi-message response parsing, invalid content type handling, and malformed frame rejection.

* HTTP/2 streaming calls are wired end-to-end: inbound DATA frames feed `cwist_grpc_decoder_feed()` directly (client-streaming handlers see messages as they arrive via the blocking `cwist_grpc_stream_recv()`), and `cwist_grpc_stream_send()` flushes DATA frames immediately from the handler thread. The app server wires this via `cwist_http2_serve_connection_ex()` + `cwist_grpc_http2_hooks()`.
* `grpc-status` / `grpc-message` are emitted in a dedicated HTTP/2 trailer HEADERS frame (END_STREAM) for both unary and streaming responses.
* `grpc-timeout` is parsed (`cwist_grpc_parse_timeout()`), enforced on streaming calls (DEADLINE_EXCEEDED trailers plus handler cancellation), and client RST_STREAM propagates to handlers via `cwist_grpc_stream_cancelled()` / a `-1` recv.
* Request metadata is normalized: header names are lowercased at the HTTP/2 layer, lookups are case-insensitive (`cwist_grpc_metadata_get()`), and `*-bin` values are base64-decoded (`cwist_grpc_metadata_get_binary()`).
* Compression negotiation: `grpc-encoding: gzip` request messages are inflated (zlib), `grpc-accept-encoding: gzip, identity` is advertised, response messages are compressed with gzip when client advertises `grpc-accept-encoding: gzip` (with `grpc-encoding: gzip` header and frame compressed flag set), and unsupported encodings are rejected with `UNIMPLEMENTED`.
* `test_grpc` covers the buffered path (unary/streaming dispatch, metadata, gzip request decompression & response compression, recv replay, timeout parsing); `test_grpc_stream` covers the wire path over h2c (split DATA delivery, trailer frames, deadline, RST cancellation, gzip request/response compression, unsupported-encoding rejection).

Known limits:

* The proto generator covers scalar, enum, repeated packed-numeric, `oneof`, `map`, and fixed-width/`double` proto3 fields plus service paths, with descriptor-set input (nested types flatten on that path); text input still lacks nested message definitions.
* The builtin health `Watch` route streams status changes over the HTTP/2 transport path and falls back to a single snapshot on the buffered dispatch path.
* Client-side hedging (`hedgingPolicy`) is not implemented — gRPC C-core does not implement it either (gRFC A6). The channel resolver supports `dns` (default), `ipv4`, and `ipv6` targets; `unix`/`unix-abstract`/`vsock` transports, `grpclb`/xDS policies, and `perAttemptRecvTimeout` remain unimplemented.

### gRPC Client Channel (v3.4)

Completed:

* `cwist_grpc_channel_connect(target, options)` resolves `dns:[//authority/]host[:port]` (default scheme, default port 443, every resolved address used), `ipv4:`/`ipv6:` literal lists, and bare `host[:port]` targets (doc/naming.md); unsupported schemes fail cleanly.
* One lazily-connected subchannel per backend address; failed dials back off per doc/connection-backoff.md (1s initial, ×1.6, 120s cap, ±0.2 jitter, reset when the SETTINGS handshake completes).
* `pick_first` (default) sticks to the connected address and fails over in resolver order; `round_robin` dials every address up front and rotates READY subchannels per call (doc/load-balancing.md). `cwist_grpc_channel_get_state()` aggregates subchannel states per the spec rules.
* gRFC A6 retry engine between the channel and the LB pick: `maxAttempts` (client-capped at 5), `initialBackoff`/`maxBackoff`/`backoffMultiplier` with ±0.2 jitter, `retryableStatusCodes` bitmask, overall call deadline shared by all attempts, per-attempt remaining `grpc-timeout`, and the `grpc-previous-rpc-attempts` header on every retry.
* Commit semantics: Response-Headers commit the RPC (no further retries); Trailers-Only failures stay retryable. Transparent retries cover RPCs that never left the client (until the deadline) and RPCs refused before server application logic (RST_STREAM REFUSED_STREAM or GOAWAY with a lower last-stream-id, one immediate retry) — neither counts against `maxAttempts` nor the throttle.
* Server pushback: `grpc-retry-pushback-ms` ≥ 0 delays the retry exactly that long (backoff restarts at `initialBackoff` afterwards); negative or unparseable values stop retries.
* `retryThrottling` token bucket per channel: retries need `token_count > maxTokens/2`; failed attempts with retryable codes (or do-not-retry pushback) cost one token, successful calls refund `tokenRatio`.
* `cwist_grpc_channel_apply_service_config_json()` parses the doc/service_config.md subset — `loadBalancingConfig`/`loadBalancingPolicy`, `methodConfig` (name matching, `retryPolicy`, `waitForReady`, `timeout`), and `retryThrottling` — with gRFC A6 validation rules.
* Error responses are Trailers-Only end-to-end (gRFC A6 / PROTOCOL-HTTP2): unary error replies are a single HEADERS frame with END_STREAM, and the streaming server delays Response-Headers until the first message, so conforming clients can retry failed calls. Servers can emit `grpc-retry-pushback-ms` via a response header (unary) or `cwist_grpc_stream_set_retry_pushback()` (streaming).
* `test_grpc_channel` covers LB stickiness/failover/rotation, retryable/non-retryable codes, `maxAttempts` exhaustion, pushback both ways, throttling, deadlines spanning attempts, JSON service config validation, GOAWAY transparent retries against a raw-socket fake, and resolver schemes; `test_grpc_stream` verifies the Trailers-Only wire forms.

---

## v3.4 Milestone (Planned)

Theme: gRPC client side, codegen completeness, and the first WASM client-side support wave. v3.3 shipped the wire-level streaming server (DATA-frame wiring, trailers, deadlines, gzip) plus the first proto codegen extension (enums, nested message fields, repeated packed numerics); v3.4 finishes the story instead of moving the already-pushed v3.3 tag.

* **`cwist proto` completion**: ~~`oneof`, `map`, fixed-width types (`fixed32/64`, `sfixed32/64`, `double`), and `protoc --descriptor_set_out` input bindings~~ (all done). CLI-only work (`tools/cli/cwist`), no library ABI impact.
* **gRPC client**: ~~h2/h2c client with unary/streaming calls, retry policy, and client-side load balancing~~ (all done: `cwist_grpc_client_*` with deadlines and cancellation, plus `cwist_grpc_channel_*` with resolver, `pick_first`/`round_robin` LB, and the gRFC A6 retry engine).
* **gRPC server leftovers**: ~~moving health `Watch` onto the streaming dispatch path~~ (done).
* **Distribution**: ~~publish the Homebrew formula~~ (done: `brew tap c4punks/cwist`, `brew install c4punks/cwist/cwist`, verified end-to-end on Linuxbrew). vcpkg stays an in-tree draft under `packaging/vcpkg/`; upstream submission postponed.
* **WASM client-side support**: bring CWIST handlers into the browser WASM ecosystem.
  * ~~In-memory HTTP dispatcher~~ ✅ (`cwist_app_dispatch_memory()`): run `cwist_app` routing and handlers directly on request/response memory buffers, with no sockets — so the same C handlers run inside a Service Worker or a JS fetch-interception layer.
  * ~~`libcwist_wasm.a` target~~ ✅ (`make wasm`): an Emscripten build that excludes the native transport (sockets, TLS, QUIC) and ships the socket-independent core — app dispatch, mux/middleware, HTTP/1 parser/serializer, query map, cJSON, memory utilities.
  * ~~`cwist_db` in-memory/blob abstraction~~ ✅ (`cwist_db_open_memory()` / `cwist_db_serialize()`): official API over `sqlite3_deserialize`-style memory buffers, so WASM apps don't need to parse custom binary blobs.
  * ~~TypedArray zero-copy serialization helpers~~ ✅ (`<cwist/wasm/typedarray.h>`): Emscripten-side macros/headers that map C struct arrays directly onto `HEAP` TypedArrays instead of round-tripping through `snprintf` JSON.

---

## v3.5 Milestone (Planned)

Theme: **Full GC — automatic resource reclamation**. Today CWIST relies on explicit destroy-family calls (`cwist_app_destroy`, connection/session teardown, `cwist_free`) paired with arena discipline. v3.5 makes cleanup automatic where it matters: threads and processes ending must close connections, and `cwist_alloc` objects must evaporate without manual frees. Design document: `docs/GC.md`.

* **`cwist_full_gc(bool)` global toggle**: when enabled, connections are closed automatically on worker-thread exit and process exit even without explicit destroy calls; when disabled (default), the current explicit model runs unchanged at zero cost.
* **Connection reclamation via libttak epoch GC** (`ttak_epoch_gc`, already wrapped by `src/core/mem/gc.c`): per-thread connection registration with epoch-deferred teardown, so a connection swept at thread exit can never UAF a thread still referencing it.
* **`cwist_alloc` registration**: allocations route into the full-GC context (internal changes to `src/core/mem/alloc.c`) so objects are reclaimed by epoch rotation instead of manual `cwist_free` calls.
* **Pseudo-RAII**: scoped guards via GCC/Clang `__attribute__((cleanup))` for handle-like locals, plus raw borrowing of LibTTAK RAII primitives.
* **Transparent `malloc` interception**: users will habitually write `malloc`, not `cwist_alloc` — so handler-thread `malloc` calls (including allocations from bundled dependencies such as cJSON) are redirected onto the worker-thread arena/epoch GC via `-Wl,--wrap=malloc` or header-level redefinition. "Write `malloc` by habit and it still evaporates at request end" is the acceptance bar.
* **Thread/process exit sweeps**: thread-local connection registry with pthread TLS destructors for worker exit, and an `atexit` sweep for process exit.

---

## Release Line & Codenames

* The 3.x line is stabilization work on the road to v4.0: release intervals are deliberately long, and each release lands a small number of large, well-tested changes rather than frequent small ones. Expect wide gaps between 3.x tags.
* The first 100% production-compatible stable release is planned as **v4.0**. Until then, minor releases may adjust public APIs (see the versioning note in the README).
* Starting with the stable line (v4.0 onward), each release receives a codename in the form **adjective + color** (e.g. "Steady Amber"). Codenames are assigned at release time and recorded here.

### Versioning rules (as practiced)

The tag history (`v0.1` → `v3.3`) settles into this convention from v3 onward, and it is the rule going forward:

* **Tags**: `v<major>.<minor>` for feature releases (`v3`, `v3.1`, `v3.2`, `v3.3`). Urgent fixes to a released tag get a patch level, `v<major>.<minor>.<patch>` (`v2.5.1`, `v2.4.1`) — patches are for hotfixes only, never for features.
* **Major** (`v2` → `v3`): a generational milestone — a broad capability jump (e.g. v3 = first reliable release, HTTP/2 stabilization). Majors are rare.
* **Minor** (`v3.2` → `v3.3`): one coherent feature theme (v3.2: HTTP/3 standards compliance + security hardening; v3.3: gRPC streaming + deferred async handlers). A minor is cut when its theme is complete, not on a calendar.
* **Release title**: `CWIST vX.Y` followed by an em-dash summary of the headline theme ("CWIST v3.3 — gRPC streaming, deferred async handlers, and stability hardening"). Pre-v3 releases used freeform subtitles ("Firefox Compatibility"); the em-dash form is the standard now.
* **Release body**: "Highlights since vX.(Y−1)" or "Major changes compared to vX.(Y−1)", grouped into numbered/sectioned items with commit references where useful.
* The 0.x line was pre-1.0 experimentation; the 1.x–2.x lines were feature accretion with themed minors. None of that constrains the 3.x rules above.

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
30. ~~**Distribution**~~ ✅ (Homebrew tap published at `c4punks/homebrew-cwist`; vcpkg kept as in-tree draft, upstream submission postponed)

---

## Contributing

If you want to pick up an item, open an issue referencing this roadmap and the specific feature number.
