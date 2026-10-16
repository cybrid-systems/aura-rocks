#!/usr/bin/env bash
# Native data plane side of docs/vs-rocksdb.md: scripts/bench_native.aura,
# sequential (idle box), REPS times per mode, same workloads and options as
# scripts/vs_rocksdb.sh / scripts/bench.aura (16-byte keys, 100-byte values,
# 64 KB memtable, L0 trigger 4, 256 KB files, 4 KB blocks, 10 bloom bits, WAL
# on without fsync, SSTs fdatasync'd like RocksDB, no compression, no cache).
# Usage: scripts/vs_native.sh N REPS OUT.jsonl MODES   (MODES: "engine op batch")
#   engine: the native driver (rkn_bench), like db_bench: no Aura per op
#   op:     Aura harness, one native call per op
#   batch:  Aura harness, one native call per 100 ops
set -uo pipefail
source "$(dirname "$0")/env.sh"
cd "$REPO_ROOT"
N=${1:-2000}; REPS=${2:-3}; OUT=${3:-build/vs_native.jsonl}; MODES=${4:-engine op batch}
for rep in $(seq 1 "$REPS"); do
  for m in $MODES; do
    BENCH_N=$N BENCH_MODE=$m BENCH_TAG="native-$m-r$rep" BENCH_OUT="$OUT" BENCH_DIR="$REPO_ROOT/build/vs-native-db" \
      "$AURA_BIN" scripts/bench_native.aura 2>&1 | grep -v '^\[aura'
  done
done
