# io_uring posted-work wake-up regression (#25)

## Scope and cause

This fixes a wake-up regression present in `dev` at
`6afc4a3a506e6740d93ebb2630de917fafd7dea9`, introduced by `7a67f87d`.
It is a partial contribution to [#25](https://github.com/c4punks/CWIST/issues/25),
not evidence that the original HTTP concurrency-64/256/512 P99.999 problem is
fully resolved.

`IORING_REGISTER_EVENTFD` sends **completion notifications from the ring to an
eventfd**. Writing that eventfd does not enqueue a CQE or wake a ring waiting
for completions. The previous code used it in the opposite direction and
removed the wake fd's input poll. With no unrelated socket activity, posted
work could therefore wait for the run loop's 100 ms shutdown-check timeout.
A registered eventfd notification also does not synthesize a CQE with
`user_data == 0`.

The fix restores the one-shot input poll and its existing callback/re-arm path
on io_uring. Empty-to-nonempty post-stack wake coalescing remains enabled.
No public API or connection layout changes are involved.

References:

- [io_uring_register(2), IORING_REGISTER_EVENTFD](https://man7.org/linux/man-pages/man2/io_uring_register.2.html)
- [io_uring_register_eventfd(3)](https://man7.org/linux/man-pages/man3/io_uring_register_eventfd.3.html)

## Regression test

`test_reactor_wake` compiles the production reactor with a test-only libc
allocator and shutdown flag. It does not replace kernel polling, eventfd,
posting, or dispatch. It needs the checked-out libttak headers but does not
link the HTTP/TLS stack or the libttak allocator runtime.

On io_uring it posts four batches of 128 nodes from a foreign thread, requires
an actual kernel completion on each batch, and runs production dispatch between
batches to exercise re-arming. A timeout cannot substitute for a completion;
a positive submission count alone is not sufficient either. An idle-ring check
rejects completion-to-eventfd feedback. On all supported backends a further 64
foreign-thread posts verify exactly-once FIFO delivery on the reactor owner.
A 30-second alarm is only a hang watchdog, not a performance acceptance gate.

```sh
git submodule update --init lib/libttak
CWIST_TEST_REQUIRE_IO_URING=1 make WERROR=1 test_reactor_wake
CWIST_REACTOR_BACKEND=epoll make WERROR=1 test_reactor_wake
CWIST_TEST_REQUIRE_IO_URING=1 make WERROR=1 SANITIZE=address,undefined test_reactor_wake
CWIST_REACTOR_BACKEND=epoll make WERROR=1 SANITIZE=address,undefined test_reactor_wake
# macOS, native kqueue:
make CC=clang WERROR=1 SANITIZE=address,undefined test_reactor_wake
```

Run Linux commands on a host that permits `io_uring_setup`; a container seccomp
policy may prohibit it. The required-backend variable makes this an explicit
failure instead of silently testing epoll. Do not retain that variable when
intentionally testing epoll. The sanitizer CI workflow explicitly tests both
Linux backends in addition to the existing full suite.

## Component measurement (2026-09-12)

Environment: Linux `6.8.0-117-generic`, GCC `13.3.0`, x86_64 Colima/QEMU guest
on an ARM64 Mac, 6 configured vCPUs and 12 GiB RAM. This is a shared, emulated
VM, not a production throughput benchmark. Each binary was built using the
same test and `-std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread`; only the reactor
source differed. `--bench` skips the deliberately failing kernel regression
gate and records `CLOCK_MONOTONIC` time from immediately before posting to
callback execution. No explicit warm-up, no sockets, one producer and one
reactor, one sequential outstanding post. The existing 100 ms run-loop timeout
was unchanged.

Three paired runs, 64 samples each per binary, ordered base/fixed,
fixed/base, base/fixed:

- Base run medians: 103.955, 103.537, 103.490 ms.
- Fixed run medians: 0.033433, 0.036471, 0.038093 ms.
- Across 192 samples per binary, base median/max: 103.787046 / 109.251727 ms.
- Across 192 samples per binary, fixed median/max: 0.034446 / 0.273538 ms.

The regression gate fails against the base with
`posted wake did not generate a completion: Timer expired` and passes after
the fix. Targeted ASan/UBSan checks pass on Linux io_uring, forced epoll, and
native macOS kqueue. These are component checks, not allocator integration,
HTTP throughput, or P99.999 measurements. The original issue's HTTP workload
and production hardware still require separate end-to-end validation.
