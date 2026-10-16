#!/usr/bin/env bash
# scripts/bench_adapt_native.aura for every static config, the native control
# plane (adaptive) and its first version (adaptive-v0), REPS times each (one
# process per run), appending to build/bench_adapt_native.jsonl.
set -uo pipefail
source "$(dirname "$0")/env.sh"
cd "$REPO_ROOT"
for rep in $(seq 1 "${REPS:-3}"); do
  for cfg in ${CFGS:-skiplist:leveled vector:tiered hash:leveled skiplist:tiered vector:leveled hash:tiered adaptive adaptive-v0}; do
    BENCH_CFG="$cfg" "$AURA_BIN" scripts/bench_adapt_native.aura 2>&1 | grep -v '^\[aura'
  done
done
