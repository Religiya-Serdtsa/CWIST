#!/usr/bin/env bash
set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH_BIN="$ROOT_DIR/benchmarks/web-frameworks/cwist_bench"

echo "Building CWIST benchmark binary..."
make -C "$ROOT_DIR" -j$(nproc) > /dev/null
make -C "$ROOT_DIR/benchmarks/web-frameworks" clean > /dev/null
make -C "$ROOT_DIR/benchmarks/web-frameworks" -j$(nproc) > /dev/null

PORT=3000
# Kill any previous instance
fuser -k $PORT/tcp 2>/dev/null || true
pkill -9 -f cwist_bench 2>/dev/null || true
sleep 1

echo "Starting CWIST on port $PORT..."
"$BENCH_BIN" &
SERVER_PID=$!

trap "kill -9 $SERVER_PID 2>/dev/null || true; pkill -9 -f cwist_bench 2>/dev/null || true" EXIT

# Wait for server ready
for i in {1..50}; do
    if curl -sf http://127.0.0.1:$PORT/ > /dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

LUA_SCRIPT="$ROOT_DIR/scripts/ci/tail_latency.lua"

echo ""
echo "=================================================="
echo " Running Local Benchmark: c=64"
echo "=================================================="
wrk -t4 -c64 -d5s -s "$LUA_SCRIPT" "http://127.0.0.1:$PORT/"

echo ""
echo "=================================================="
echo " Running Local Benchmark: c=256"
echo "=================================================="
wrk -t8 -c256 -d5s -s "$LUA_SCRIPT" "http://127.0.0.1:$PORT/"

echo ""
echo "=================================================="
echo " Running Local Benchmark: c=512"
echo "=================================================="
wrk -t12 -c512 -d5s -s "$LUA_SCRIPT" "http://127.0.0.1:$PORT/"

echo ""
echo "Benchmark completed successfully."
