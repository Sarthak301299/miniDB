#!/bin/bash
# Pinned scaling benchmark: every process gets its own core.
#
#   core 0            : benchmark (all client threads share this one core)
#   core 1            : launcher
#   core 2 .. S+1     : server 0 .. S-1   (each started with --threads=1)
#
# A configuration with S servers needs S+2 cores; configs that don't fit are
# skipped (never oversubscribed), so numbers are never silently contaminated by
# processes sharing a core.
#
# Usage: ./run_bench.sh [mixed|readonly] [runs_per_config]
#   CORES=<n>   override detected core count (for dry runs)
#   DRY_RUN=1   print the pinning plan without running anything
#
# Run from the project root with ./launcher ./server ./benchmark already built.
set -u
MODE=${1:-mixed}
RUNS=${2:-3}
CORES=${CORES:-$(nproc)}
CLIENTS=${CLIENTS:-8}
QPC=${QPC:-500}
ROOT=$(pwd)

case "$MODE" in
  mixed)    EXTRA="" ;;
  readonly) EXTRA="--read-only=true" ;;
  *) echo "mode must be mixed or readonly"; exit 1 ;;
esac

max_servers=$((CORES - 2))
if [ "$max_servers" -lt 1 ]; then
  echo "Only $CORES core(s): need at least 3 (benchmark + launcher + 1 server)."; exit 2
fi

for S in 1 2 3 4 5 6 7 8; do
  if [ "$S" -gt "$max_servers" ]; then
    echo "servers=$S: SKIPPED (needs $((S+2)) cores, have $CORES)"; continue
  fi
  echo "servers=$S: benchmark->core 0, launcher->core 1, servers->cores 2..$((S+1))"
  [ "${DRY_RUN:-0}" = "1" ] && continue
  for r in $(seq 1 "$RUNS"); do
    d=$(mktemp -d /tmp/pinned.XXXXXX)
    base=$((30000 + (RANDOM % 2000) * 10))
    taskset -c 1 "$ROOT/launcher" --servers="$S" --threads=1 --data-dir="$d" \
        --base-port="$base" --client-port=$((base + 9)) > "$d/launcher.log" 2>&1 &
    LP=$!
    # wait for every server to exist, then pin each to its own core
    for i in $(seq 0 $((S - 1))); do
      for _ in $(seq 1 100); do
        pid=$(pgrep -f "server --id=$i --data-dir=$d" | head -1)
        [ -n "$pid" ] && break; sleep 0.1
      done
      taskset -a -cp $((2 + i)) "$pid" > /dev/null
    done
    sleep 2
    out=$(taskset -c 0 timeout 120 "$ROOT/benchmark" --port=$((base + 9)) \
          --clients="$CLIENTS" --queries-per-client="$QPC" --seed-rows=150 $EXTRA 2>&1)
    kill -9 "$LP" 2>/dev/null
    pkill -9 -f "server --id=.* --data-dir=$d" 2>/dev/null
    echo "  run $r: $(echo "$out" | grep -o 'errors=[0-9]*') $(echo "$out" | grep -o 'throughput_qps=[0-9.]*')"
    rm -rf "$d"
  done
done