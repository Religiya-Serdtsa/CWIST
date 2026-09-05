# CWIST Step-by-Step Tutorials

Welcome to the **CWIST** hands-on tutorial series. Each directory contains a runnable C application (`main.c`), a build configuration (`CMakeLists.txt`), and a detailed English guide (`README.md`).

---

## 📚 Complete Tutorial Index

1. **[01-hello-world](01-hello-world)**: Creating a minimal CWIST HTTP web server and registering GET route handlers.
2. **[02-routing-params](02-routing-params)**: Dynamic route parameters (`/users/:id`) and path variable extraction.
3. **[03-json-api](03-json-api)**: Building RESTful JSON APIs using `cJSON` and setting response content headers.
4. **[04-middleware-pipeline](04-middleware-pipeline)**: Pipeline middleware registration (`cwist_app_use`) for global logging and request processing.
5. **[05-database-sqlite](05-database-sqlite)**: Embedded in-memory SQLite integration, schema creation, and querying.
6. **[06-db-pool](06-db-pool)**: High-concurrency thread-safe SQLite connection pool (`cwist_db_pool_t`) management.
7. **[07-jwt-auth](07-jwt-auth)**: Issuing and signing JSON Web Tokens (`cwist_jwt_sign`) for stateless authentication.
8. **[08-sse-realtime](08-sse-realtime)**: Unidirectional real-time event streaming with Server-Sent Events (SSE).
9. **[09-websocket-chat](09-websocket-chat)**: Full-duplex bidirectional communication with WebSocket handlers.
10. **[10-background-scheduler](10-background-scheduler)**: Asynchronous task execution with worker thread background schedulers.
11. **[11-csrf-protection](11-csrf-protection)**: Generating and verifying CSRF defense tokens.
12. **[12-cookie-session](12-cookie-session)**: Stateful HTTP cookie and session lifecycle management.
13. **[13-compression-gzip-zstd](13-compression-gzip-zstd)**: Payload compression using Gzip and Zstandard.
14. **[14-metrics-observability](14-metrics-observability)**: Exposing Prometheus-compatible operational metrics `/metrics`.
15. **[15-multipart-upload](15-multipart-upload)**: Parsing `multipart/form-data` and handling file uploads.
16. **[16-html-builder](16-html-builder)**: Programmatic HTML document tree generation with the HTML builder API.
17. **[17-css-composer](17-css-composer)**: Dynamic CSS stylesheet generation and composition.
18. **[18-healthz-check](18-healthz-check)**: Implementing Kubernetes and load balancer `/healthz` health check endpoints.
19. **[19-post-quantum-tls](19-post-quantum-tls)**: Enabling Post-Quantum Hybrid TLS (X25519MLKEM768) powered by BoringSSL.
20. **[20-http3-quic-server](20-http3-quic-server)**: Running HTTP/3 servers over QUIC transport via lsquic.
21. **[21-waf-security](21-waf-security)**: Inspecting request payloads for malicious patterns using built-in WAF rules.
22. **[22-rate-limiting](22-rate-limiting)**: Protecting endpoints with throughput rate-limiting middleware.
23. **[23-cors-policy](23-cors-policy)**: Configuring Cross-Origin Resource Sharing (CORS) response headers.
24. **[24-secure-headers](24-secure-headers)**: Hardening web responses with X-Frame-Options and Content Security Policy headers.
25. **[25-big-dumb-reply-cache](25-big-dumb-reply-cache)**: Ultra-fast static response caching with the Big Dumb Reply (BDR) engine.
26. **[26-generic-orm](26-generic-orm)**: Type-safe generic ORM query building via C11 `_Generic` macros.
27. **[27-db-migrations](27-db-migrations)**: Programmatic database schema migration execution.
28. **[28-transparent-column-encryption](28-transparent-column-encryption)**: Transparent column-level database encryption (`db_crypt`).
29. **[29-test-client-automation](29-test-client-automation)**: Writing automated in-memory integration tests with `cwist_test_client`.
30. **[30-graceful-shutdown](30-graceful-shutdown)**: Handling SIGTERM/SIGINT signals for graceful server shutdown.

---

## 🚀 Quick Start

Build any tutorial using CMake:

```bash
cd tutorials/01-hello-world
mkdir build && cd build
cmake ..
make
./tut01
```

---

## Where to go next

- Finished the series? Combine subsystems with the task-oriented
  [guides](../docs/tutorials/) (CRUD blog, NATS, WebTransport).
- Need a signature? Jump to the [API module index](../docs/API.md) or the
  [flat quick reference](../docs/api-quickref.md).
- Want bigger apps? Browse the runnable [examples](../example/) and the
  production deployment [fly.board](https://github.com/gg582/fly.board).
- Full map: [docs/README.md](../docs/README.md).
