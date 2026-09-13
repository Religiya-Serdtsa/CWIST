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
| Warmup | `wrk -t12 -c400 -d10s http://127.0.0.1:$PORT/` | Discarded. Lets JIT-tiered runtimes (JVM) reach steady state. Applied identically to all four servers. |
| Measurement | `wrk -t12 -c400 -d10s http://127.0.0.1:$PORT/` | Recorded: `Requests/sec`, avg `Latency`. |

**CPU budget:** every server and the load generator see the full runner CPU set — no pinning, no per-runtime thread caps. Each runtime uses its own default/auto worker sizing (CWIST `CWIST_WORKERS=auto`, Tokio `available_parallelism`, Go `GOMAXPROCS=default`, Netty `ioWorkerCount=nproc`). This keeps the comparison fair: no framework gets a hand-tuned advantage the others do not get.

- Server startup wait is a readiness loop (`curl` poll, up to 90s for the JVM), not a fixed sleep.
- **Peak RSS** is sampled from `ps -o rss=` immediately after the measured run.
- **Context switches** (`nvcsw + nivcsw` from `ps`) are counted only over the measured
  window — the counter baseline is taken *after* warmup.

### Tuned low-latency run

CWIST and Spring Boot are each measured a second time under `wrk -t4 -c100 -d10s`
(lower concurrency than the main `-t12 -c400` profile above), each in its own
dedicated process — for Spring Boot this is a fresh boot replaying the same
trained AOT cache used for its main run, so it isn't paying a second cold-start
penalty CWIST doesn't pay either. This is CWIST's own published low-latency
profile; giving Spring Boot the identical treatment keeps the "tuned" numbers
comparable instead of showing CWIST's best case next to a number Spring was
never measured at. Axum and Gin are not yet included in this second pass.

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
  "wrk_profile": "wrk -t12 -c400 -d10s (after 10s warmup, warmup discarded)",
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

## Latency distribution chart

`docs/webserver-latency-distribution.svg` (linked in the README right below the
bar-chart trends SVG) plots each server's latency distribution as a density
curve, so the *shape* of the tail is visible at a glance instead of only its
P99.999 number. It compares the same five servers as the summary table above
(`cwist`, `cwist_c1m`, `axum`, `gin`, `spring`) — the `_tuned` entries (different,
lower-concurrency load profile) and the opt-in experimental A/Bs
(`cwist_c1m_arena1`, `cwist_sharded`) are excluded so the chart only ever compares
runs made under the identical `wrk -t12 -c400 -d10s` profile.

This is **reconstructed, not raw**: wrk only ever reports percentiles
(min/p50/p75/p90/p99/p99.9/p99.99/p99.999/max — all now captured by the
workflow's `parse_wrk()`, see `.github/workflows/bsd-kqueue-benchmarks.yml`),
never the underlying per-request samples. `scripts/ci/benchmark.py`
(`_inverse_cdf_samples`) linearly interpolates the inverse CDF between those
known percentile points to synthesize a representative sample set, then runs a
standard Gaussian KDE (`_gaussian_kde`, Silverman's rule of thumb for
bandwidth) over it. The x-axis uses `log1p(ms)` so a long tail (Gin, Spring
Boot) doesn't compress the tighter CWIST/Axum curves into an unreadable spike
at the left edge. Treat the curve shapes as representative, not exact — a
server with sparser percentile data (an older history row missing the newer
p75/p999/p9999/min/max fields) still renders, just with fewer anchor points to
interpolate between.

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
