#!/usr/bin/env bash
# Workload only. benchmark_session.py owns server/descendant cleanup.
set -eu
PID=${BENCHMARK_SERVER_PID:?missing supervised server PID}
PORT=$1
TXT=$2
STAT_TXT=$3
WAIT=$4
THREADS=$5
CONNECTIONS=$6
for i in $(seq 1 $WAIT); do
  curl -sf http://127.0.0.1:$PORT/ > /dev/null 2>&1 && break
  sleep 1
done

# Warmup (results discarded)
wrk -t"$THREADS" -c"$CONNECTIONS" -d10s http://127.0.0.1:$PORT/ > /dev/null 2>&1 || true

# Initial stat (after warmup)
BEFORE_CSW=$(ps -o nvcsw=,nivcsw= -p $PID 2>/dev/null | awk '{print $1+$2}')
[ -z "$BEFORE_CSW" ] && BEFORE_CSW=0

wrk -t"$THREADS" -c"$CONNECTIONS" -d10s -s "$GITHUB_WORKSPACE/scripts/ci/tail_latency.lua" --latency http://127.0.0.1:$PORT/ > $TXT 2>&1 || true

AFTER_CSW=$(ps -o nvcsw=,nivcsw= -p $PID 2>/dev/null | awk '{print $1+$2}')
[ -z "$AFTER_CSW" ] && AFTER_CSW=0
RSS=$(ps -o rss= -p $PID 2>/dev/null | awk '{print $1}')
[ -z "$RSS" ] && RSS=0

CSW=$((AFTER_CSW - BEFORE_CSW))
[ $CSW -lt 0 ] && CSW=0

echo "rss_kib=$RSS csw=$CSW" > $STAT_TXT
cat $TXT
