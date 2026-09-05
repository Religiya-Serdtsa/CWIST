# CWIST Documentation Map

Everything you need to learn, use, and extend CWIST, in suggested reading order.

## 1. Getting started

* [README](../README.md) — install, hello world, feature tour, benchmarks.
* [Tutorial series](../tutorials/README.md) — 30 hands-on modules; each directory
  ships a runnable `main.c`, a `CMakeLists.txt`, and a step-by-step guide.
  Start at [01-hello-world](../tutorials/01-hello-world/README.md) and follow
  the numbering; later modules assume earlier concepts.

## 2. Task-oriented guides

Longer walkthroughs that combine several subsystems:

* [Build a CRUD blog with CWIST](tutorials/blog-crud.md) — routing + SQLite + JSON.
* [NATS integration](tutorials/nats-integration.md) — messaging from handlers.
* [WebTransport server](tutorials/webtransport-server.md) — experimental HTTP/3
  datagram/streams API.
* [CWIST tutorial (single page)](tutorial/cwist_tutorial.md) — the condensed
  one-file variant.

## 3. API reference

* [API module index](API.md) — per-module documentation under `docs/api/`.
* [Flat quick reference](api-quickref.md) — one-file listing of public calls.
* [REFERENCE.md](REFERENCE.md) — architecture and behavioral reference.
* [Doxygen HTML](https://religiya-serdtsa.github.io/CWIST/) — generated from
  annotated public headers (`include/cwist/`).

## 4. Runnable examples

Self-contained demos under [`example/`](../example/):

| Example | Demonstrates |
|---------|--------------|
| [simple-server](../example/simple-server) | Minimal HTTP server |
| [http](../example/http) | Step-by-step HTTP features |
| [db](../example/db) / [db-crypt](../example/db-crypt) | SQLite integration, transparent column encryption |
| [jwt](../example/jwt) | Token issuing and verification |
| [websocket / othello-web](../example/othello-web) | WebSocket game server |
| [rps-showcase](../example/rps-showcase) | Static caching + throughput demo |
| [json-builder](../example/json-builder), [html](../example/html), [template](../example/template) | Rendering helpers |
| [micro](../example/micro), [mem](../example/mem), [siphash](../example/siphash), [sstring](../example/sstring) | Core utilities |
| [cde-json-viewer](../example/cde-json-viewer) | JSON viewer app |
| [webtransport](../example/webtransport) | Experimental WebTransport |

## 5. Benchmarks and methodology

* [Web server benchmark methodology](webserver-benchmark.md) — how the CI
  numbers in the README are produced.
* [benchmark-trends.svg](benchmark-trends.svg) / [webserver-benchmark-trends.svg](webserver-benchmark-trends.svg) —
  historical CI trends.

## 6. References

* [Mux router algorithm notes](references/mux_algorithm_references.md)
* [ROADMAP.md](../ROADMAP.md) — feature status and the v3.4 milestone plan.
