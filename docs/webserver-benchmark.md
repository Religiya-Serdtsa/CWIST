# Web Server Benchmark Methodology

This document describes exactly how the CWIST vs Axum vs Spring Boot comparison in
`benchmarks/webserver.json` (rendered into `docs/webserver-benchmark-trends.svg`) is
produced, so every published number can be reproduced and audited.

The suite runs in GitHub Actions (`web-server-benchmark` job in
`.github/workflows/bsd-kqueue-benchmarks.yml`) on `ubuntu-latest`. All three servers are
minimal `GET / -> "Hello, World!"` applications generated inline by the workflow; no
framework-specific tuning is applied beyond what is documented here.

## Load profile

| Phase | Command | Purpose |
|---|---|---|
| Warmup | `wrk -t12 -c400 -d10s http://127.0.0.1:$PORT/` | Discarded. Lets JIT-tiered runtimes (JVM) reach steady state. Applied identically to all three servers. |
| Measurement | `wrk -t12 -c400 -d10s http://127.0.0.1:$PORT/` | Recorded: `Requests/sec`, avg `Latency`. |

- Server startup wait is a readiness loop (`curl` poll, up to 90s for the JVM), not a fixed sleep.
- **Peak RSS** is sampled from `ps -o rss=` immediately after the measured run.
- **Context switches** (`nvcsw + nivcsw` from `ps`) are counted only over the measured
  window — the counter baseline is taken *after* warmup.

## CWIST

- Built from the checked-out commit: `make`, then the bench server linked against
  `libcwist.a` with `gcc -O3`.
- Listens on port `8081`.

## Axum

- `axum = "0.7"`, `tokio = "1"` (`full` features), `cargo build --release`.
- Listens on port `8082`.

## Spring Boot (JVM fairness configuration)

The JVM is not a measure-once runtime: tiered JIT compilation, heap resizing, and class
loading dominate short runs. The suite therefore fixes and *records* the following:

| Setting | Value |
|---|---|
| Java | Temurin **21** (`actions/setup-java`) |
| Spring Boot | **3.2.3** (`spring-boot-starter-web`) |
| Virtual threads | **enabled** — `spring.threads.virtual.enabled=true` (Project Loom) |
| JVM options | `-Xms512m -Xmx512m` (fixed heap, no resize noise during measurement) |
| AOT cache | **CDS** — training run with `-XX:ArchiveClassesAtExit=app.jsa` (clean shutdown via SIGTERM), measured run replays `-XX:SharedArchiveFile=app.jsa` |
| Warmup | 10s `wrk` run, discarded (see above) |
| Port | `8080` |

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
  "spring_env": {
    "java_version": "openjdk version \"21.x\" ... (Temurin)",
    "spring_boot_version": "3.2.3",
    "jvm_opts": "-Xms512m -Xmx512m -XX:SharedArchiveFile=... (CDS AOT cache)",
    "virtual_threads": true,
    "aot_cache": "CDS (-XX:ArchiveClassesAtExit training run + -XX:SharedArchiveFile replay)"
  }
}
```

`scripts/ci/benchmark.py render` prints this environment block as the SVG footer and in
the README benchmark summary.

## Known limitations

- GitHub-hosted runners are shared, noisy 4-vCPU machines; treat absolute numbers as
  trend data, not lab-grade measurements.
- RSS for the JVM includes its reserved heap by design (`-Xms512m`); this is a real
  cost of the runtime model and is reported as-is.
- The workload measures plain-text routing throughput only — no TLS, JSON
  serialization, or database access.
