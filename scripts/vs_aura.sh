#!/usr/bin/env bash
# aura-rocks side of docs/vs-rocksdb.md: scripts/bench.aura, sequential (idle box), REPS times per mode.
# Usage: scripts/vs_aura.sh N REPS OUT.jsonl MODES   (MODES: "pure sink")
#   pure: no kernels;  sink: the KEEP set of the last policy run installed (BENCH_KEEP), no policy run
set -uo pipefail
source "$(dirname "$0")/env.sh"
cd "$REPO_ROOT"
N=${1:-2000}; REPS=${2:-3}; OUT=${3:-build/vs_aura.jsonl}; MODES=${4:-pure sink}
KEEP=rk-checksum,rk-block-seek,rk-pad,rk-entry-encode,rk-wal-record,rk-block-build,rk-bloom-build,rk-block-get,rk-cache-sim,rk-sst-build,rk-sst-merge,rk-scan
for rep in $(seq 1 "$REPS"); do
  for m in $MODES; do
    k=""; [ "$m" = sink ] && k=$KEEP
    BENCH_N=$N BENCH_KEEP=$k BENCH_TAG="aura-$m-r$rep" BENCH_OUT="$OUT" BENCH_DIR="$REPO_ROOT/build/vs-db" \
      "$AURA_BIN" scripts/bench.aura 2>&1 | grep -v '^\[aura'
  done
done
