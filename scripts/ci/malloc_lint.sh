#!/usr/bin/env bash
# Raw-allocator lint: cwist's own src/ is expected to route heap
# allocation through cwist_alloc()/cwist_free() (or CWIST_DEFER_FREE /
# cwist_alloc_scoped() for block-scoped locals) rather than bare
# malloc/calloc/realloc/free/strdup/strndup -- that's the only path
# cwist_full_gc()'s per-job/thread-exit auto-sweep can see.
#
# A sizeable set of existing call sites legitimately bypass that (WASM
# fallback path, the allocator's own bookkeeping, thread-payload structs
# handed to pthread_create that must outlive any cwist_alloc scope,
# lsquic/zlib/brotli/zstd codec state, etc.) -- rather than block on that
# pre-existing set, this gate diffs against a pinned baseline
# (scripts/ci/malloc-baseline.txt) and fails only on NEW raw allocator
# calls. Same shape as scripts/ci/h2spec_gate.sh's baseline diff.
#
# The actual scan/diff logic now lives in `cwist audit --gate` (kept in
# one place instead of duplicated between a shell script and the CLI);
# this stays as a thin, stable entry point for anything (CI, docs, muscle
# memory) that already points at this path.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec "$ROOT/tools/cli/cwist" audit --gate "$ROOT" --baseline "$ROOT/scripts/ci/malloc-baseline.txt"
