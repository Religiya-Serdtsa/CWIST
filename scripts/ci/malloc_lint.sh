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
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BASELINE="$(dirname "$0")/malloc-baseline.txt"
CURRENT="$(mktemp)"
trap 'rm -f "$CURRENT"' EXIT

grep -rnE '(^|[^a-zA-Z0-9_])(malloc|calloc|realloc|free|strdup|strndup)[[:space:]]*\(' \
    "$ROOT/src" --include='*.c' \
  | grep -vE 'cwist_(malloc|calloc|realloc|free|strdup|strndup|alloc)' \
  | awk -F: '{
        content = $0
        sub(/^[^:]*:[^:]*:/, "", content)
        if (content !~ /^[[:space:]]*(\/\/|\*|\/\*)/) print
    }' \
  | sed "s#^$ROOT/##" \
  | sort -u > "$CURRENT"

NEW="$(comm -13 "$BASELINE" "$CURRENT")"
FIXED="$(comm -23 "$BASELINE" "$CURRENT")"

status=0
if [ -n "$NEW" ]; then
    echo "NEW raw allocator call(s) in src/ -- use cwist_alloc()/cwist_free()"
    echo "or CWIST_DEFER_FREE instead so cwist_full_gc() can see them. If"
    echo "this call site genuinely needs the raw allocator (add a comment"
    echo "explaining why), add its line to scripts/ci/malloc-baseline.txt:"
    echo "$NEW"
    status=1
fi
if [ -n "$FIXED" ]; then
    echo "NOTE: baseline entries no longer present (tighten scripts/ci/malloc-baseline.txt):"
    echo "$FIXED"
fi
exit $status
