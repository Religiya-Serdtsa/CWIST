# Full GC: Automatic Resource Reclamation in CWIST

Status: **implemented, opt-in** (`src/core/mem/gc.c`,
`include/cwist/core/mem/gc.h`). `cwist_full_gc(true)` is a real, callable
toggle today, not a future one — see Tutorial 30
(`tutorials/30-graceful-shutdown/`) for a minimal example. All six design
sections below, including section 5 (transparent `malloc` interception),
are implemented and tested. This document remains the design contract for
what the mode does and does not cover; the explicit destroy-family model
stays fully supported (and is the default) whether or not full-GC mode is
ever enabled.

## Motivation

CWIST today relies on disciplined explicit cleanup: `cwist_app_destroy()`,
connection/session teardown at the transport layer, and `cwist_free()` paired
with request arenas. Two gaps remain:

1. **Thread/process exit does not close connections.** If a worker thread (or
   the whole process) ends without the explicit destroy path running, its
   connections linger until the peer or the OS gives up on them.
2. **`cwist_alloc` still requires manual `cwist_free`.** Miss one and the
   object leaks for the life of its owner.

Full-GC mode (`cwist_full_gc(true)`) closes both gaps. When disabled (the
default), the current explicit model runs unchanged at zero cost.

## Design

### 1. `cwist_full_gc(bool)` — the global toggle

```c
cwist_full_gc(true);   /* enable automatic reclamation */
```

One process-wide switch. **This cuts across `cwist_multiport_get_app()`'s
per-port sub-applications.** Multiport advertises detached ports as
"independently tunable sub-applications," but that independence is
app-level config only (routes, middleware, TLS, size limits) -- it was
never true of process-wide subsystems, and `cwist_full_gc()` is one.
Several `cwist_app` instances can share a process (that's the whole point
of multiport), but they cannot have different full-GC settings: enabling
it from any one of them enables it for all of them, with no per-app
opt-out, because the toggle, the pending-sweep bookkeeping, and the
epoch-retire pipeline it drives are all singletons scoped to the process,
not to a `cwist_app *`. (The cJSON allocator hook installed at process
start via a constructor function is the same shape, for the same reason:
there's one cJSON allocator per process, not per app.) A deployment that
genuinely needs different memory-management behavior for different
sub-apps needs separate processes -- separate `cwist_app` instances in one
process cannot get it.

When enabled:

- Connections are closed automatically on **worker-thread exit** and on
  **process exit**, even when no explicit destroy-family call was made.
- `cwist_alloc` objects are registered with the reclamation engine and freed
  by epoch rotation instead of manual `cwist_free` calls.

When disabled, none of the tracking machinery is active; the explicit model
keeps its current performance profile.

### 2. Reclamation engine: libttak epoch GC

The engine is libttak's epoch GC (`ttak_epoch_gc`, see
`lib/libttak/include/ttak/mem/epoch_gc.h`), already wrapped by CWIST in
`src/core/mem/gc.c` (`cwist_gc`, `cwist_reg_ptr`, `cwist_gc_rotate`).

- Each worker thread registers its live connections and `cwist_alloc` blocks
  against the current epoch.
- Reclamation is **epoch-deferred**: a swept object is not reclaimed until
  the epoch rotates past every thread that could still reference it. A
  connection closed at thread exit can therefore never use-after-free a
  thread that is mid-request on it.
- Rotation can be automatic (background rotate thread with adaptive cadence
  driven by allocation hints) or manual (`cwist_gc_rotate` from the event
  loop), matching the existing wrapper semantics.

### 3. Thread/process exit sweeps

- **Thread exit**: a per-thread connection registry lives in thread-local
  storage; a pthread TLS destructor sweeps and closes whatever the thread
  still owns when it exits.
- **Process exit**: an `atexit` sweep performs the same close-out for the
  whole process, so stray connections are shut (TLS shutdown, socket close,
  session teardown) rather than abandoned.

### 4. Pseudo-RAII for handle-like locals

For stack-scoped handles, CWIST provides pseudo-RAII guards:

- GCC/Clang `__attribute__((cleanup))` scoped guards that run the matching
  teardown when the variable leaves scope — C's practical equivalent of
  RAII.
- Raw borrowing of LibTTAK RAII primitives where the cleanup-attribute trick
  is unavailable.

### 5. Transparent `malloc` interception

Users will habitually write `malloc`, not `cwist_alloc` — depending on
finger discipline is how leak-free claims fail. `<cwist/core/mem/intercept.h>`
makes the two spellings equivalent, opt-in per translation unit:

```c
#define CWIST_INTERCEPT_MALLOC
#include <cwist/core/mem/intercept.h>
#include <cwist/app.h>

static void handle(cwist_http_request *req, cwist_http_response *res) {
    char *buf = malloc(256);   /* -> cwist_malloc_shim() */
    ...
    /* no free(buf) needed: under cwist_full_gc(true), this evaporates
     * exactly like a forgotten cwist_alloc() would. free(buf), if called,
     * still works normally either way. */
}
```

- **cJSON's own internal allocations** are separately redirected onto
  `cwist_alloc` via `cJSON_InitHooks` (`src/core/mem/alloc.c`) — this was
  already true before the shim below existed and needs no opt-in.
- **A handler's own `malloc`/`calloc`/`realloc`/`free`** are redirected by
  `cwist_malloc_shim`/`cwist_calloc_shim`/`cwist_realloc_shim`/`cwist_free_shim`
  (`src/core/mem/alloc.c`) once the translation unit defines
  `CWIST_INTERCEPT_MALLOC` before including the header. `malloc`/`calloc`
  keep their real semantics (uninitialized vs. zeroed memory respectively —
  neither becomes `cwist_alloc`'s always-zeroed behavior) and register with
  the pending-sweep list only when `cwist_full_gc_enabled()`; `realloc`
  preserves or drops tracking based on whether the incoming pointer was
  tracked, never opting an untracked pointer in on its own; `free` untracks
  first (a safe no-op if the pointer was never tracked) then calls the real
  `free`.

**Why header-scoped, not link-level `-Wl,--wrap=malloc`**: the link-level
wrap intercepts *every* `malloc` reference in the final binary, including
inside vendored dependencies (BoringSSL, lsquic, cnats, sqlite3) that never
see CWIST's headers and have no reason to expect a non-libc allocator
underneath them. This is not a hypothetical risk: BoringSSL calls
`OPENSSL_cleanse()` to zero secret key material on free, on the assumption
of plain heap semantics — redirecting its allocations into the
epoch-deferred GC arena would mean cleanse-then-free no longer reliably
erases the memory before it becomes reclaimable, a security regression,
not a performance footnote. A header-scoped `#define` only ever takes
effect in a translation unit that explicitly opts in, and vendored
dependencies structurally never do (they don't include CWIST headers at
all) — this excludes them by construction. The header also pulls in
`<stdlib.h>` before defining the macros, specifically so a later
`#include <stdlib.h>` (this translation unit's own, or transitively via
another header) is a no-op against its own include guard instead of
re-declaring `malloc`/`calloc`/`realloc`/`free` through the now-active
macros — which reliably fails to compile otherwise.

**Reclaim cadence** reuses the existing `cwist_gc_scope_track`/
`cwist_gc_scope_flush` pipeline unchanged (per-thread tracking, thread-exit
TLS sweep as the last resort, explicit flush at natural completion points
such as the per-job flush already wired into `src/sys/io/io_queue.c`) — the
same mechanism `cwist_alloc()` already uses, not a separate policy. A
policy that reclaimed only at thread exit would be close to a no-op for
the C1M reactor's intentionally long-lived worker threads, exactly the
deployment model this feature is meant to help most.

**Cross-thread handoff** reuses the existing `cwist_gc_scope_disown()`
escape hatch (see below and `tests/test_full_gc_ownership_handoff.c`) — a
pointer that legitimately needs to outlive its allocating scope (cached in
a connection pool, handed to a background job) calls this once at the
handoff point and becomes the new owner's responsibility. No separate API
exists for shimmed pointers; they use the same one `cwist_alloc()`
pointers do.

**Measured overhead** (`tests/bench_malloc_intercept.c`, single-threaded,
mixed 16–512 byte allocations with an occasional `realloc`, this machine):

| configuration | ns/op | vs. baseline |
|---|---:|---:|
| baseline (no shim) | ~11.8 | — |
| shim, full-GC off (default) | ~12.2 | +3% |
| shim, full-GC on | ~21.9 | +86% |

The "off" cost is one relaxed atomic load (`cwist_full_gc_enabled()`) plus
one extra function call per operation — matching the overhead
`cwist_alloc()`/`cwist_free()` already pay today. The "on" cost is the real
price of the safety net: pending-sweep list insertion/removal on every
`malloc`/`free`. Both are per-call microbenchmark numbers on a single
thread — they isolate per-operation overhead, not lock contention under
concurrent load.

### 6. `cwist_alloc` internals

`src/core/mem/alloc.c` gains a registration hook: under full-GC mode every
`cwist_alloc` block is recorded with the epoch GC (size-aware via
`cwist_reg_ptr_sized`) so epoch rotation reclaims it; explicit `cwist_free`
remains correct and simply unregisters the block early.

## Semantics summary

| Event | Without full_gc (default) | With `cwist_full_gc(true)` |
|---|---|---|
| Worker thread exits | Connections must be closed explicitly | TLS sweep closes owned connections |
| Process exits | `cwist_app_destroy()` required | `atexit` sweep closes remaining connections |
| `cwist_alloc` object | Manual `cwist_free` | Epoch rotation reclaims; explicit free still fine |
| cJSON's internal `malloc` | N/A (cJSON manages its own memory) | Redirected to `cwist_alloc` via `cJSON_InitHooks`; freed by epoch rotation |
| Handler calls bare `malloc()` (opt-in via `CWIST_INTERCEPT_MALLOC`) | Heap leak if forgotten | Redirected to the pending-sweep list; freed by epoch rotation |
| Teardown safety | Caller discipline | Epoch-deferred; no reclaim while referenced |

## Non-goals and notes

- Full-GC mode is **opt-in**; the default build keeps the explicit model with
  zero tracking overhead.
- This is not a tracing garbage collector: reclamation is epoch-based and
  scoped to framework-managed resources (connections, `cwist_alloc` blocks,
  handler-context `malloc`).
- Kernel-level resources (file descriptors, TLS sessions) are closed
  deterministically by the exit sweeps; the epoch deferral governs only the
  memory reclamation behind them.
