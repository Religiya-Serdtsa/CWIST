# Web Server Benchmark Methodology

This document describes exactly how the CWIST vs Axum vs Gin vs Spring Boot comparison in
`benchmarks/webserver.json` (rendered into `docs/webserver-benchmark-trends.svg`) is
produced, so every published number can be reproduced and audited.

The suite runs in GitHub Actions (`web-server-benchmark` job in
`.github/workflows/bsd-kqueue-benchmarks.yml`) on `ubuntu-latest`. All four servers are
minimal `GET / -> "Hello, World!"` applications generated inline by the workflow; no
framework-specific tuning is applied beyond what is documented here.

## Load profile

| Phase | Command | Purpose |
|---|---|---|
| Warmup | `taskset -c $WRK_CPUS wrk -t$WRK_T -c400 -d10s http://127.0.0.1:$PORT/` | Discarded. Lets JIT-tiered runtimes (JVM) reach steady state. Applied identically to all four servers. |
| Measurement | `taskset -c $WRK_CPUS wrk -t$WRK_T -c400 -d10s http://127.0.0.1:$PORT/` | Recorded: `Requests/sec`, avg `Latency`. |

**CPU budget split (anti-oversubscription):** the runner's cores are split into two disjoint sets — servers are pinned via `taskset` to the first `SRV = nproc - max(2, nproc/3)` cores, `wrk` is pinned to the remaining `WRK_T = max(2, nproc/3)` cores and runs with `-t$WRK_T` threads. Each runtime's worker count is sized to its pinned set (`CWIST_WORKERS=$SRV`, `TOKIO_WORKER_THREADS=$SRV`, `GOMAXPROCS=$SRV`, `reactor.netty.ioWorkerCount=$SRV`). Pinning server and load generator to disjoint cores is what keeps the latency tail flat; sharing the same cores inflates average latency with pure scheduling jitter.

- Server startup wait is a readiness loop (`curl` poll, up to 90s for the JVM), not a fixed sleep.
- **Peak RSS** is sampled from `ps -o rss=` immediately after the measured run.
- **Context switches** (`nvcsw + nivcsw` from `ps`) are counted only over the measured
  window — the counter baseline is taken *after* warmup.

## CWIST

- Built from the checked-out commit: `make`, then the bench server linked against
  `libcwist.a` with `gcc -O3`.
- Listens on port `9091`.

## Axum

- `axum = "0.7"`, `tokio = "1"` (`full` features), `cargo build --release`.
- Listens on port `9092`.

## Gin (Go)

- `github.com/gin-gonic/gin` **v1.10.0** on the Go toolchain provisioned by
  `actions/setup-go` (Go **1.22**), `go build` with `gin.ReleaseMode` and the
  default `net/http` server underneath.
- Listens on port `9094`.

## Spring Boot (JVM fairness configuration)

The JVM is not a measure-once runtime: tiered JIT compilation, heap resizing, and class
loading dominate short runs. The suite therefore fixes and *records* the following.

The benchmarked stack is **Spring WebFlux on Reactor Netty** — Spring's reactive,
event-loop server — rather than Spring MVC on the thread-per-request Tomcat servlet
container. Netty's event loop is the appropriate Java comparison point for the async
CWIST (io_uring/epoll) and Axum (tokio) servers. On top of the event loop,
**virtual threads are enabled** (`spring.threads.virtual.enabled=true`, Project Loom)
so that any work dispatched off the Netty event loop runs on Loom virtual threads
instead of a bounded platform-thread pool.

| Setting | Value |
|---|---|
| Java | Temurin **21** (`actions/setup-java`) |
| Spring Boot | **3.2.3** (`spring-boot-starter-webflux`) |
| Server | **Reactor Netty** (event loop, non-blocking I/O) |
| Virtual threads | **enabled** — `spring.threads.virtual.enabled=true` (Project Loom) |
| JVM options | `-Xms512m -Xmx512m` (fixed heap, no resize noise during measurement) |
| AOT cache | **CDS** — training run with `-XX:ArchiveClassesAtExit=app.jsa` (clean shutdown via SIGTERM), measured run replays `-XX:SharedArchiveFile=app.jsa` |
| Warmup | 10s `wrk` run, discarded (see above) |
| Port | `9093` |

The full Spring run command is:

```
java -Xms512m -Xmx512m -XX:SharedArchiveFile=app.jsa -jar spring-bench-0.0.1-SNAPSHOT.jar
```

## Recorded metadata

Every measurement appended to `benchmarks/webserver.json` carries the runtime facts next
to the numbers, so a result is never an isolated req/s figure:

```json
{
  "wrk_profile": "wrk -t$WRK_T -c400 -d10s pinned to dedicated cores (after 10s warmup, warmup discarded)",
  "go_env": {
    "go_version": "go version go1.22.x linux/amd64",
    "framework": "Gin v1.10.0 (gin-gonic/gin, release mode)"
  },
  "spring_env": {
    "java_version": "openjdk version \"21.x\" ... (Temurin)",
    "spring_boot_version": "3.2.3",
    "stack": "Spring WebFlux + Reactor Netty (event loop, virtual threads enabled)",
    "jvm_opts": "-Xms512m -Xmx512m -XX:SharedArchiveFile=... (CDS AOT cache)",
    "virtual_threads": true,
    "aot_cache": "CDS (-XX:ArchiveClassesAtExit training run + -XX:SharedArchiveFile replay)"
  }
}
```

`scripts/ci/benchmark.py render` prints this environment block as the SVG footer and in
the README benchmark summary.

## Known limitations

- GitHub-hosted runners are shared, noisy **4-vCPU** machines; treat absolute numbers as
  trend data, not lab-grade measurements. Each entry records `runner_hw` (vCPU count and
  CPU model) precisely so that entries from different machines are never compared
  directly — e.g. CWIST measures ~170k-185k req/s on a local 12-thread Ryzen 5600X but
  ~89k req/s on the 4-vCPU CI runner under the identical wrk profile. A drop between
  entries with different `runner_hw` reflects the machine, not the code.
- RSS for the JVM includes its reserved heap by design (`-Xms512m`); this is a real
  cost of the runtime model and is reported as-is.
- The workload measures plain-text routing throughput only — no TLS, JSON
  serialization, or database access.
