<p align="center">
  <img src="./logo.png" alt="CWIST logo" width="220">
</p>

<h1 align="center">CWIST</h1>
<p align="center"><strong>C Web development Is Still Trustworthy</strong></p>

<p align="center">
  A modern, lightweight C web framework for secure and scalable applications.
</p>

---

CWIST is a modern, lightweight C web framework designed for building secure and scalable applications. It brings the ergonomics of modern web frameworks to C without sacrificing performance or control.

## Features

- **HTTP/1.1 Server**: Robust request parsing and response handling.
- **SString**: Custom string library with compare and substr support.
- **WebSocket Support**: Easy upgrade from HTTP to persistent connections.
- **Middleware System**: Chainable processing for logging and security.
- **Path Parameters**: Express-style routing with `:param` support and Mux Router.
- **JSON Builder**: Lightweight utility using cJSON for response building.
- **Static Assets & DB Sharing**: Serve directories with `cwist_app_static` and reuse SQLite handles via `cwist_app_use_db`.
- **Nuke DB**: High-performance in-memory SQLite with persistent disk synchronization via `cwist_nuke_init`.
- **Secure by Design**: Built-in integration with Monocypher.

## Quick Start

### 1. Installation

```sh
git clone https://github.com/gg582/cwist.git
cd cwist
make install

2. Hello World

#include <cwist/sys/app/app.h>
#include <cwist/core/sstring/sstring.h>

void index_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "Hello from CWIST!");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use_db(app, ":memory:");
    cwist_app_get(app, "/", index_handler);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}

3. Compilation

gcc -o server main.c -lcwist -lssl -lcrypto -luriparser -lcjson -ldl -lpthread
./server

Nuke DB

Nuke DB is a read-optimal persistent store.

It follows a Read-from-RAM, Write-to-Disk philosophy.

Ultra-Fast Reads: All queries execute against an in-memory SQLite instance, providing sub-millisecond response times for read-heavy workloads.

Reliable Writes: Every COMMIT triggers an immediate background synchronization to disk using WAL mode.

Trade-off: Write performance is slightly affected by disk synchronization, but reads remain consistently fast.

Raw Bootstrap: On startup the on-disk SQLite file is read directly as a raw image using sqlite3_deserialize.

Integrity Guard: Each bootstrap runs PRAGMA integrity_check and aborts if corruption is detected.

Safe Fallback: When the disk-to-memory transfer fails the system switches to the disk handle.


Usage

1. cwist_nuke_init("data.db", 5000);
Loads data.db to memory and enables auto-sync every 5 seconds plus immediate sync on commit.


2. Use cwist_nuke_get_db() to access the in-memory handle.


3. On application exit (SIGINT or SIGTERM) Nuke DB performs a final synchronization.


4. Safety
If the initial load from disk fails due to corruption, Nuke DB enters read-only disk protection mode.



LibTTAK Performance Core

CWIST links against the in-tree lib/libttak build and exposes the subsystems that landed with the latest drop.

Generational Arena Allocator

Static assets (cwist_app_static) and Big Dumb Reply blobs are staged in a tracked arena backed by ttak_mem_tree.

Each generation is released in one shot, preventing RSS fragmentation.

Cache-aligned chunks keep hot endpoints from repeatedly calling malloc and free.


Epoch-Based Reclamation (EBR)

Threads pin critical sections with ttak_epoch_enter and ttak_epoch_exit.

Stale buffers are reclaimed once no worker remains in the previous epoch.

CWIST exposes cwist_gc(), cwist_gc_shutdown(), and cwist_reg_ptr() to integrate libttak GC easily.


Detachable Memory

ttak_detachable_mem_alloc layers a small-cache and signal-safe arena on top of EBR.

Used for TLS write buffers, WebSocket frames, and zero-copy HTTP responses.


Lock-Free Job Queue

cwist_io_queue mirrors libttak’s lock-free queue design.

Producers push jobs using a single atomic swap without mutex contention.

Consumers reuse detached nodes as sentinels to prevent fragmentation.


Big Dumb Reply Guardrails

Entries expire after roughly five minutes or about 100k hits.

Default payload budget is capped at 32 MiB.

Old blobs are trimmed automatically.


RPS Showcase Example

example/rps-showcase demonstrates a high-throughput configuration.

Run the benchmark:

wrk -t4 -c128 -d30s http://127.0.0.1:8080/rps

The showcase ships with its own notes in example/rps-showcase/README.md and is meant to be the reproducible throughput entry point for the repository.

Benchmark Snapshot

Recorded ApacheBench sample (see BENCHMARK.txt for the full transcript):

Tool: ApacheBench 2.3

Command: ab -n 100 -c 85 -k http://localhost:31744/

Requests per second: 2958.40 [#/sec] (mean)

Mean time per request: 28.732 ms

Mean time across all concurrent requests: 0.338 ms

Failed requests: 0


Benchmark host profile captured from the active container:

Kernel: Linux 6.14.0-1017-azure x86_64

CPU allocation: cpuset 0-3 (4 visible logical CPUs via nproc and lscpu)

Processor: AMD EPYC 7763 64-Core Processor

Topology: 1 socket, 2 cores, 2 threads per core

Observed clock during capture: 3.23 GHz to 3.31 GHz

Scheduler clock tick: 100 Hz (getconf CLK_TCK)

Reported throttling: none in cpu.stat during capture


Suggested throughput workflow:

1. Build and run example/rps-showcase.


2. Warm the server with a short wrk or ab run.


3. Capture the exact container, kernel, CPU, and timer details shown above.


4. Save the raw tool transcript next to the environment notes in BENCHMARK.txt.



This keeps the benchmark notes useful as a performance reference instead of a single isolated requests-per-second number.

Dependencies

cJSON

OpenSSL (libssl, libcrypto)

uriparser

libttak (https://github.com/gg582/libttak)

SQLite3


Past Roadmap

Security: CORS middleware and origin/header policy management

RestAPI: Dedicated features for optimized REST API server deployment


Documentation Style

The repository keeps a terminal-friendly plain-text documentation tone where useful.
For GitHub presentation, this README.md acts as the project landing page.
