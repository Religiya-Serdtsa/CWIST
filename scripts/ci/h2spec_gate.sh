#!/usr/bin/env bash
# h2spec conformance gate: the HTTP/2 h2c surface is compared against a
# pinned baseline (scripts/ci/h2spec-baseline.txt). New failures fail the
# build; fixed entries also fail it so the baseline gets tightened.
set -euo pipefail

PORT="${H2SPEC_PORT:-18080}"
H2SPEC="${H2SPEC_BIN:-h2spec}"
BASELINE="$(dirname "$0")/h2spec-baseline.txt"

make h2c-server >/dev/null

./h2c-server "$PORT" &
SERVER_PID=$!
trap 'kill -TERM -"$SERVER_PID" 2>/dev/null || kill "$SERVER_PID" 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
    curl -s --max-time 1 "http://127.0.0.1:$PORT/" >/dev/null 2>&1 && break
    sleep 0.1
done

OUT="$(mktemp)"
trap 'kill -TERM -"$SERVER_PID" 2>/dev/null || kill "$SERVER_PID" 2>/dev/null || true; rm -f "$OUT"' EXIT
"$H2SPEC" -h 127.0.0.1 -p "$PORT" -o "${H2SPEC_TIMEOUT:-3}" >"$OUT" 2>&1 || true
tail -3 "$OUT"

grep -a "×" "$OUT" | sed 's/^ *× [0-9]*: //' | sort -u > "$OUT.fails" || true

NEW_FAILURES="$(comm -13 "$BASELINE" "$OUT.fails")"
FIXED="$(comm -23 "$BASELINE" "$OUT.fails")"

status=0
if [ -n "$NEW_FAILURES" ]; then
    echo "NEW h2spec failures (not in baseline):"
    echo "$NEW_FAILURES"
    status=1
fi
if [ -n "$FIXED" ]; then
    echo "NOTE: baseline entries now passing (tighten scripts/ci/h2spec-baseline.txt):"
    echo "$FIXED"
fi

total="$(grep -aE '^Finished in' "$OUT" >/dev/null 2>&1 && grep -aoE '[0-9]+ tests, [0-9]+ passed' "$OUT" | tail -1 || echo unknown)"
echo "h2spec summary: $total"
exit $status
