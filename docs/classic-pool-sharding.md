# Classic pool: optional lock sharding (`CWIST_POOL_SHARDS`)

Status: **experimental, opt-in, default off.** Related to [issue #25](../../../issues/25)
and [classic-pool-starvation.md](classic-pool-starvation.md) (PR #38), but a
distinct concern - see "What this is not" below.

## The question this answers

While reviewing PR #38's `pending > idle` starvation fix, a follow-up
question came up: could per-worker OS-scheduling variance (a signalled
worker slow to actually resume) still cause unfairness under a single
shared queue? Tracing the code answered that directly: the classic pool has
no per-worker dispatch to be unfair about. Every idle worker blocks on the
same `pthread_cond_t` and pulls from the head of the same shared FIFO; the
starvation PR #38 fixed was about aggregate queue depth vs. aggregate idle
count, not about which specific thread gets picked, and stays fixed
regardless of any one worker's scheduling delay.

That left a different, real question: does a single global mutex protecting
one shared queue become a submitter-side bottleneck of its own at high
submitter fan-out, independent of the starvation race? This document is the
answer to that question, not the fairness one.

## What this is not

- **Not a fairness fix.** It does not change which worker services which
  task, and does not touch the `pending > idle` decision from PR #38.
- **Not a fix for issue #25's original P99.999 benchmark number.** Like
  PR #37's reactor fix, this targets a different bottleneck than the one
  the-benchmarker's `GET /` workload exercises; it is not expected to move
  that number, and is not validated against it below.

## The change

`CWIST_POOL_SHARDS=N` (default 1, i.e. no-op) splits the single
`head`/`tail`/`lock`/`cond`/counters into N independent shards. Submitters
round-robin across shards via one global atomic counter (even distribution,
not per-submitter-thread affinity); each shard is otherwise the exact same
code path as before (`http_dynamic_worker_thread`, `http_spawn_worker`,
`cwist_http_pool_submit`, all parameterized on a `pool` pointer instead of
a single global). Shard 0 is always the pre-existing static `g_dyn_pool` -
the default `N=1` path is the original code, unchanged in a byte-identical
sense (no extra atomic, no extra branch taken on the hot path).

Trade-off: N independent FIFOs instead of one means cross-shard ordering is
no longer strictly FIFO (a request submitted later on one shard can be
serviced before one submitted earlier on another). The starvation guarantee
from PR #38 still holds *per shard*, so this does not reintroduce the bug
it fixed.

## Measurements (local, not CI)

A synthetic microbenchmark (`tests/bench_pool_contention.c`, not part of
`make test`) isolates the submitter-side lock: N submitter threads
hammering `cwist_http_pool_submit()` with a handler that does ~50 integer
ops and returns (no syscall, so the lock/queue path dominates). Run on this
machine (12 cores), 32 submitter threads, 30,000 submits/thread:

| CWIST_POOL_SHARDS | throughput (ops/s) | vs. N=1 |
|---:|---:|---:|
| 1 (baseline)  | ~288,000–301,000 | 1.0x |
| 2             | ~348,000–360,000 | ~1.2x |
| 4             | ~454,000–494,000 | ~1.6x |
| 8             | ~568,000–627,000 | ~2.1x |
| 12            | ~648,000–736,000 | ~2.4x |

Each row is 3 runs; the ranges above are the observed spread, not error
bars. This is a real, repeatable local effect - the global lock is
measurably the bottleneck at this submitter fan-out on this hardware - but
it is **not** a CI-backed, HTTP-level result. It has not been run through
the-benchmarker-style workload or the repo's CI benchmark workflow, so it
says nothing about end-to-end RPS or tail latency yet. Treat the table as
"the hypothesis is worth taking further," not as a shipped performance
claim.

## Correctness

`tests/test_pool_sharding.c` (part of `make test`) exercises init/submit/
destroy across shard counts (1, 4, 8, 64) including edge cases (shard count
not a divisor of thread count, shard count exceeding worker/submitter
count), asserting every submitted task runs exactly once. Both this test
and the pre-existing `test_classic_pool_scaling.c` pass clean under
`SANITIZE=address,undefined`.

That ASan run caught a real bug during development worth recording: the
first version of this change freed the sharded pool array in
`cwist_http_pool_destroy()` immediately after broadcasting shutdown,
without waiting for detached worker threads to actually wake and return -
a worker parked in `pthread_cond_wait` on a shard about to be freed raced
the free and produced a heap-use-after-free. The fix makes each worker
thread (for a heap-allocated shard only - shard 0's static `g_dyn_pool` is
never freed, matching pre-existing behavior and the assertion in
`test_classic_pool_scaling.c`) decrement `active_workers` as the last thing
it does before returning, and `destroy()` waits (bounded to 5s, logging a
warning if exceeded) on that counter per extra shard before freeing
anything.

## Open questions before this graduates past "experimental"

- Real CI benchmark run (this repo's `bench.sh`/GitHub Actions workflow) to
  see whether the local microbenchmark's contention effect survives contact
  with actual HTTP parsing/handling overhead, which may dwarf the
  lock-acquire cost this measures in isolation.
- A sensible default `N` (cores? `g_http_thread_count`-derived? left to the
  operator?) if this is ever promoted out of opt-in-only.
- Whether round-robin submission's loss of strict cross-request FIFO
  ordering matters for any real workload (soft-realtime keep-alive
  scheduling, request affinity, etc.).
