# HTTP/1.1 pipeline fairness and framing

Related: [issue #25](https://github.com/Religiya-Serdtsa/CWIST/issues/25).

## Scope

The event-driven HTTP/1.1 handler serves at most **16 complete requests per
invocation**. Buffered requests are continued through the reactor's FIFO post
queue, not by recursively calling the handler or waiting for another read event.
This bounds request dispatch count, not elapsed time: a blocking handler, large
body, or synchronous file-stream operation can still occupy a reactor.

The nonblocking parser's preliminary pass receives only the header block. The
validated Content-Length/chunked assembler supplies the body afterward. This
avoids copying subsequent pipeline messages into a body-less request and doing
that unnecessary copy again for each request in a long pipeline.

Half-close is preserved across continuations. Complete buffered requests are
answered after the peer shuts down its write side; an incomplete trailing
request is closed rather than rearmed forever. Pool destruction also prohibits
new continuations from deferred completions in the final reactor drain.

## Behavioral regression tests

```sh
make WERROR=1 test_http_pipeline test_http_fairness test_async_defer test_http_chunked
```

`test_http_pipeline` checks no-body GET, Content-Length: 0, a positive length,
and chunked framing, with another request in the same stash.

`test_http_fairness` gates the first pipeline handler, makes a second connection
ready on the same reactor, then releases the gate. It checks:

- the probe is served before all 256 bulk requests finish;
- each application-handler invocation serves no more than 16 bulk requests;
- all 256 response frames and bodies arrive in order without more client input;
- complete and truncated-header half-close variants still drain valid requests;
- a real deferred completion in pool destruction sends its response and closes
  without queuing an orphan continuation, even while the global run flag is true.

The assertions use service order, framing, and lifecycle rather than latency
thresholds. Watchdog/poll timeouts only prevent a broken test from hanging.
The Makefile runs these variants in the full suite, and the macOS focused CI
job explicitly runs both new test targets.

The dev baseline served the probe only after **256/256** requests and exposed
later pipeline bytes as a GET body. The initial fairness implementation exposed
two additional lifecycle regressions (half-close truncation and a post created
inside the final destruction drain); both have dedicated regressions.

## Performance claim boundary

These tests establish removal of a specific head-of-line blocking mechanism.
They do **not** establish resolution of the original issue's 645.72 ms P99.999.

A separate closed-loop comparison of the repository's `GET /` benchmark app used
Linux x86_64, GCC 13.3, wrk 4.1.0, one process/reactor, two client threads,
concurrency 64/256/512, 2 s warmup, 10 s samples, and three rounds per variant.
All 18 valid runs had zero transport/status errors. Throughput was similar;
extreme percentile latency did not consistently improve. The experiment used
the fairness/framing candidate before its final teardown-only guard and is
not a final-release performance acceptance gate.

Do not close #25 based on these results alone. Its source benchmark uses a
different load generator; reproduce the original command, revision, worker
settings, resource limits, and host conditions before making a general tail
latency claim. P99.999 also needs enough observations and repeated runs.

For comparable local runs, raise the **soft** descriptor limit before starting
both client and server (within the existing hard limit), e.g. `ulimit -n 65536`.
A 1024 soft limit made the initial diagnostic run hit CWIST's inflight admission
limit and return 503s; those samples were discarded, not counted as latency wins.

## Internal API compatibility

Consumers allocating `cwist_http_async_conn_t` must rebuild for its appended
`peer_eof` field. `cwist_http_async_conn_fill()` now returns zero for orderly EOF
and records it in that field; callers drain complete buffered requests and then
close. Buffered `cwist_http_async_rearm()` schedules work instead of executing it
inline. Its existing failure contract still consumes/closes the connection.
