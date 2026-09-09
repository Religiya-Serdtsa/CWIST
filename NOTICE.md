# Third-Party Components

CWIST vendors the following dependencies under `lib/`. Each retains its own
license; copies live in the respective submodule directories. This file is a
summary for convenience — the authoritative text is always the upstream
license file.

| Component | Path | License | Notes |
|-----------|------|---------|-------|
| BoringSSL | `lib/boringssl` | Apache-2.0 | TLS 1.3, ECH, hybrid PQC KEM |
| lsquic | `lib/lsquic` | MIT | QUIC / HTTP/3 engine; some proto-quic-derived parts are BSD-3-Clause (Chromium Authors, see `LICENSE.chrome`) |
| nghttp3 | `lib/nghttp3` | MIT | HTTP/3 framing (submodule) |
| ngtcp2 | `lib/ngtcp2` | MIT | QUIC transport helpers (submodule) |
| libttak | `lib/libttak` | BSD-3-Clause | Memory/epoch/token-bucket utilities |
| SQLite | `lib/sqlite3` | Public Domain | Embedded database; author disclaims copyright |
| cJSON | `lib/cjson` | MIT | JSON parsing |
| cnats | `lib/cnats` | Apache-2.0 | NATS client |
| uriparser | `lib/uriparser` | BSD-3-Clause | URI parsing; only the library is built/linked — its test suite (LGPL-2.1-or-later) and fuzzing code (Apache-2.0) are not |
| multipart-parser-c | `lib/multipart-parser-c` | MIT | `multipart/form-data` parsing |
| Monocypher | `lib/monocypher` | BSD-2-Clause OR CC0-1.0 (dual, your choice) | Crypto primitives |

When distributing `libcwist.a` or linked binaries, review the linked set:
static linking propagates the license obligations of every component above
(notably the Apache-2.0 patent grant and NOTICE terms for BoringSSL and
cnats, and the attribution clauses of the BSD-licensed components).
