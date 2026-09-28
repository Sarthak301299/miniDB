#!/bin/bash
# Pinned scaling benchmark: every process gets its own core.
#
#   cores 0 .. C-1        : benchmark (all client threads share these C cores)
#   cores C .. C+L-1      : launcher  (L cores; it runs one thread per client)
#   cores C+L .. C+L+S-1  : servers, ONE core each (each started with --threads=1)
#
# Defaults C=3, L=4 (override CLIENT_CORES / LAUNCHER_CORES). WHY THE LAUNCHER
# AND CLIENTS GET SEVERAL CORES: measured per-query CPU on a read-only workload
# is ~39us in the launcher (constant, independent of server count), ~15us in
# the clients, and ~50us across the servers. Pinning the launcher to ONE core
# would cap total throughput near 1/39us = ~25k QPS no matter how many servers
# you add, and pinning the clients to one core caps near ~65k QPS -- so a
# "1 core per process" layout measures the harness, not the database.
#
# A configuration with S servers needs C+L+S cores; configs that don't fit are
# skipped (never oversubscribed), so numbers are never silently contaminated by
# processes sharing a core.
#
# Usage: ./run_bench.sh [mixed|readonly] [runs_per_config]
#   CORES=<n>            override detected core count (for dry runs)
#   SERVER_THREADS=<n>   worker threads per server process (default 1). Servers stay pinned to ONE core each;
#                        extra threads only overlap time spent BLOCKED (e.g. the primary waiting for replica ACKs).
#   DRY_RUN=1   print the pinning plan without running anything
#
# Compare replica read modes:  MINIDB_REPLICA_MODE=wal (default) vs MINIDB_REPLICA_MODE=flush
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

# ---- port selection -------------------------------------------------------
# Each run needs a block of 10 consecutive ports (server query/control ports plus
# the launcher's client port). They MUST be below the kernel's ephemeral range:
# outgoing connections take their source ports from there (default 32768-60999),
# and a benchmark with many clients holds hundreds of them, so a server whose
# fixed port falls inside that range can find it already taken and bind() fails.
# (An earlier version picked from 30000-49999 and ~25% of read-only runs died
# that way.) Blocks are also probed and skipped if anything is listening.
EPH_LO=$(cut -f1 /proc/sys/net/ipv4/ip_local_port_range 2>/dev/null || echo 32768)
PORT_HI=${PORT_HI:-$((EPH_LO - 100))}
PORT_LO=${PORT_LO:-$((PORT_HI - 12000))}
[ "$PORT_LO" -lt 1100 ] && PORT_LO=1100
port_busy() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }   # true if something listens
pick_base() {
  local tries=0 b p ok
  while [ "$tries" -lt 200 ]; do
    tries=$((tries + 1))
    b=$(( PORT_LO + (RANDOM % ((PORT_HI - PORT_LO) / 10)) * 10 ))
    ok=1
    for p in $(seq "$b" $((b + 9))); do port_busy "$p" && { ok=0; break; }; done
    [ "$ok" = 1 ] && { echo "$b"; return 0; }
  done
  return 1
}

CC=${CLIENT_CORES:-1}
LC=${LAUNCHER_CORES:-1}
max_servers=$((CORES - CC - LC))
if [ "$max_servers" -lt 1 ]; then
  echo "Only $CORES core(s): need at least $((CC + LC + 1)) (clients $CC + launcher $LC + 1 server)."; exit 2
fi

for S in 1 2 3 4 5 6 7 8; do
  if [ "$S" -gt "$max_servers" ]; then
    echo "servers=$S: SKIPPED (needs $((S+CC+LC)) cores, have $CORES)"; continue
  fi
  echo "servers=$S: clients->cores 0-$((CC-1)), launcher->cores $CC-$((CC+LC-1)), servers->cores $((CC+LC))..$((CC+LC+S-1))"
  [ "${DRY_RUN:-0}" = "1" ] && continue
  for r in $(seq 1 "$RUNS"); do
    attempt=0; result=""
    while [ "$attempt" -lt 3 ]; do
      attempt=$((attempt + 1))
      d=$(mktemp -d /tmp/pinned.XXXXXX)
      base=$(pick_base) || { result="INVALID (no free port block found)"; break; }
      taskset -c $CC-$((CC+LC-1)) "$ROOT/launcher" --servers="$S" --threads="${SERVER_THREADS:-1}" --data-dir="$d" \
          --base-port="$base" --client-port=$((base + 9)) > "$d/launcher.log" 2>&1 &
      LP=$!
      # 1) Wait until the launcher reports it is serving clients (it only says so
      #    after every server answered PING and one was promoted).
      ready=0
      for _ in $(seq 1 300); do
        grep -q "listening for clients" "$d/launcher.log" 2>/dev/null && { ready=1; break; }
        kill -0 "$LP" 2>/dev/null || break
        sleep 0.1
      done
      # 2) Pin each server to its own core. Every server MUST be found: an
      #    unpinned run would silently measure the wrong thing.
      pinned_ok=1
      if [ "$ready" = "1" ]; then
        for i in $(seq 0 $((S - 1))); do
          pid=""
          for _ in $(seq 1 50); do
            pid=$(pgrep -f "server --id=$i --data-dir=$d" | head -1)
            [ -n "$pid" ] && break; sleep 0.1
          done
          if [ -z "$pid" ] || ! taskset -a -cp $((CC + LC + i)) "$pid" > /dev/null 2>&1; then pinned_ok=0; fi
        done
      fi
      if [ "$ready" != "1" ] || [ "$pinned_ok" != "1" ]; then
        why=$(grep -iE "bind|fatal|could not|error" "$d/launcher.log" 2>/dev/null | head -1 | cut -c1-90)
        kill -9 "$LP" 2>/dev/null; wait "$LP" 2>/dev/null
        pkill -9 -f "server --id=.* --data-dir=$d" 2>/dev/null
        result="INVALID (ready=$ready pinned=$pinned_ok; ${why:-see $d/launcher.log})"
        [ "$attempt" -lt 3 ] && { rm -rf "$d"; continue; }   # bad start: retry on a fresh port block
        break                                                # keep $d for inspection
      fi
      out=$(taskset -c 0-$((CC-1)) timeout 120 "$ROOT/benchmark" --port=$((base + 9)) \
            --clients="$CLIENTS" --queries-per-client="$QPC" --seed-rows=150 $EXTRA 2>&1)
      kill -9 "$LP" 2>/dev/null; wait "$LP" 2>/dev/null   # 'wait' silences bash's "Killed" job message
      pkill -9 -f "server --id=.* --data-dir=$d" 2>/dev/null
      result="$(echo "$out" | grep -o 'errors=[0-9]*') $(echo "$out" | grep -o 'throughput_qps=[0-9.]*')"
      [ "$attempt" -gt 1 ] && result="$result  (after $attempt start attempts)"
      rm -rf "$d"
      break
    done
    echo "  run $r: $result"
  done
done