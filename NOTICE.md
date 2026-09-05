# Third-Party Components

CWIST vendors the following dependencies under `lib/`. Each retains its own
license; copies live in the respective submodule directories. This file is a
summary for convenience — the authoritative text is always the upstream
license file.

| Component | Path | License | Notes |
|-----------|------|---------|-------|
| BoringSSL | `lib/boringssl` | OpenSSL/ISC-style (see `LICENSE`) | TLS 1.3, ECH, hybrid PQC KEM |
| lsquic | `lib/lsquic` | MIT | QUIC / HTTP/3 engine |
| nghttp3 | `lib/nghttp3` | MIT | HTTP/3 framing (submodule) |
| ngtcp2 | `lib/ngtcp2` | MIT | QUIC transport helpers (submodule) |
| libttak | `lib/libttak` | BSD-style (see `LICENSE`) | Memory/epoch/token-bucket utilities |
| SQLite | `lib/sqlite3` | Public Domain | Embedded database |
| cJSON | `lib/cjson` | MIT | JSON parsing |
| cnats | `lib/cnats` | Apache-2.0 | NATS client |
| uriparser | `lib/uriparser` | BSD-3-Clause | URI parsing |
| multipart-parser-c | `lib/multipart-parser-c` | MIT (upstream) | `multipart/form-data` parsing |

When distributing `libcwist.a` or linked binaries, review the linked set:
static linking propagates the license obligations of every component above
(notably Apache-2.0 patent and NOTICE terms for BoringSSL/cnats/uriparser
heritage files).
