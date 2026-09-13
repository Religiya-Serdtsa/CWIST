# Classic pool burst starvation (#25 follow-up)

This is a liveness repair, not a claim that the original HTTP P99.999 report
has been resolved. It is independent of the io_uring wake-up repair in #37.

## Cause and change

A condition-variable worker remains counted as idle until it wakes and
reacquires the queue mutex. Previously, submission only grew the classic
pool when `idle_workers == 0`. A burst could queue several connections while
one signalled worker still counted idle; once it started a long-lived
keep-alive handler, the remaining connections had no worker and no further
submission to trigger growth.

Submission now compares **queued demand against idle capacity** under the
queue mutex. The maximum-worker check and thread reservation occur in that
same critical section, so concurrent submitters cannot reserve the same
remaining slot. It still attempts at most one growth per submission and
keeps the existing two-attempt thread-creation failure rollback.

No pool defaults, public interfaces, reactor backend, allocator, or pipeline
budget change. Failed creation can still leave work for existing workers;
this does not promise progress when resources are exhausted and every
existing handler is blocked. Existing detached-worker shutdown behavior is
outside this repair.

## Deterministic regression

`make WERROR=1 test_classic_pool_scaling` runs four separate processes:

- One delayed idle worker and eight held handlers: all eight must start
  **before any handler completes**. The old code starts only one.
- Three delayed idle workers: the burst still needs eight concurrent entries.
- Concurrent submitters with a four-worker cap: no cap overshoot; all eight
  tasks finish once the four active handlers are released.
- Two injected `pthread_create` failures: one reservation rolls back, seven
  handlers can start, and all eight finish after release.

The harness executes production queue code. It delays the condition-wait
reacquisition and makes captured worker threads joinable solely for safe test
cleanup; it does not claim to test production detached-worker shutdown.
The test is included in the normal `make test` suite.

## Validation status

On the local ARM64 Linux environment, the `-Werror` build succeeded. Running
all 57 Makefile test targets with `make -k WERROR=1 test` produced 56 passes
and one failure: `test_grpc.c:243`, the `append_grpc_string_frame` append
assertion. Rebuilding with the original `dev` HTTP source reproduced the same
failure. This is an observed pre-existing failure in this environment, not a
claim that its underlying cause is understood or that the full suite is green.

A clean ASan/UBSan build passed these six targets, with leak detection enabled
and the repository's existing LSan suppression file:
`test_classic_pool_scaling`, `test_http`, `test_http_pipeline`,
`test_http_fairness`, `test_async_defer`, and `test_conn_registry`.
The four pool modes all passed. Independent static review found no blocking
candidate defect. Hosted CI must still be reviewed; this remains a draft
until the outstanding validation concerns are resolved.

## HTTP measurements and limits

Local measurements used a dedicated ARM64 Linux VM (4 vCPUs, 4 GiB, kernel
6.8.0-117), the repository's fixed empty-GET benchmark, four classic worker
processes, two load-generator threads, and an FD limit of 65536. Each measured
20-second run followed 5 seconds of discarded warmup. Three paired rounds at
64, 256 and 512 connections produced 18 error-free runs after correcting the
measurement clock. Both sides had #37 applied; this patch changes only the
classic pool, which does not use that reactor wake-up path.

wrk 4.2.0 (`a211dd5a7050b1f9e8a9870b95513060e72ac4a0`) was built locally
with its `time_us()` using `clock_gettime(CLOCK_MONOTONIC)` instead of
`gettimeofday`. Initial system-wrk results were discarded: Lima clock-sync
logs showed backwards wall-clock steps, and wrk's unsigned latency subtraction
can classify those as histogram-overflow timeouts. This is a measurement-tool
adjustment, not a server optimization. wrk's reported percentiles also include
its built-in synthetic latency correction, not only raw observations.

P99.999 in milliseconds, all three runs (before → after):

- c64: `3.194, 3.185, 3.168` → `5.576, 3.172, 3.160`
- c256: `3.707, 3.583, 3.573` → `3.690, 3.623, 3.616`
- c512: `4.256, 4.172, 4.173` → `4.276, 4.165, 4.266`

**No consistent steady-state P99.999 reduction was established.** The c64
candidate outlier is retained, not hidden. Throughput ranges overlap. Separate
uncontrolled real-HTTP connection bursts also completed on both versions;
those runs did not reproduce the held-worker schedule from the deterministic
test. Completed-request histograms alone cannot prove progress for requests
that never complete. The original issue's 645.72 ms result has not been
reproduced with this different hardware, revision and workload setup.

References: [wrk timer and timeout accounting](https://github.com/wg/wrk/blob/4.2.0/src/wrk.c),
[wrk statistics correction](https://github.com/wg/wrk/blob/4.2.0/src/stats.c).
