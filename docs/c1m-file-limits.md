# C1M file limits

This is a partial fix for issue #25.

## Cause

The old code sets both file limits to 1050000 before it calls `setrlimit`.
If the call fails, the second call uses the value that the code already changed.
The code loses the original hard-limit value.

In the test container, the soft limit stayed at 1024. The hard limit was 524288.
The low soft limit caused a low C1M connection limit.
With one process and one HTTP worker, eight of 40 open connections received HTTP 503.

## Change

The code increases the soft limit only within the existing hard limit.
It does not change the hard limit or decrease a higher soft limit.
A failed system call produces an error message, not a success message.

With this change, the test container used a soft limit of 524288.
All 40 connections received HTTP 200. The hard limit did not change.

## Checks

- Three child-process checks use real file limits.
- Eight system-call checks cover equal, higher, zero, and unlimited values, plus errors.
- The test checks remain active with `NDEBUG`.
- ASan and UBSan checks cover resource limits, HTTP, HTTP pipelining, and deferred work.
- The Linux ARM64 test run passed 58 of 59 Make targets.
  `test_grpc` failed at `append_grpc_string_frame` on both base `d08caace` and this change.

This change prevents unnecessary HTTP 503 responses under these file limits.
It does not, by itself, prove a P99.999 latency gain.
