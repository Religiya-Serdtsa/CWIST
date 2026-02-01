# CWIST RPS Showcase

This example demonstrates how CWIST's libttak-powered memory runtime keeps latency flat while serving millions of requests per second.

## Highlights

- **Detachable arenas**: `/rps` streams a JSON payload that lives inside a tracked libttak detachable allocation. No per-request copies.
- **Epoch-Based Reclamation (EBR)**: each worker thread pins the current payload snapshot via `ttak_epoch_enter/exit`. Refreshing the payload retires the old arena generation without pausing inflight requests.
- **Generational swaps**: `/refresh` rebuilds the snapshot with new metadata and atomically swaps it while `ttak_epoch_retire` + `ttak_epoch_reclaim` defer cleanup.
- **Live telemetry**: a background stats thread prints the observed requests-per-second so you can correlate tooling output (`wrk`, `ab`, `hey`) with CWIST's internal counters.

## Build & Run

```bash
cd example/rps-showcase
make
./rps_showcase
```

Then exercise the RPS route:

```bash
wrk -t4 -c128 -d30s http://127.0.0.1:8080/rps
```

You should see both `wrk` and the example's `[stats]` line report stable throughput once the detachable arena warms up.

## Endpoints

| Route      | Description |
|------------|-------------|
| `GET /`    | Prints usage instructions. |
| `GET /rps` | Returns a zero-copy JSON payload pinned by libttak and streamed directly to the socket. |
| `GET /refresh` | Rebuilds the payload and retires the previous arena generation; accepts an optional query string to describe the reason. |

Use `/refresh?reason=bench` while a benchmark is running to see how EBR keeps requests flowing even when the payload swaps.
