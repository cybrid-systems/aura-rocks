#!/usr/bin/env bash
# Run scripts/bench_cache.aura for every static cache config and the adaptive
# controller, REPS times each, appending to build/bench_cache.jsonl.
set -uo pipefail
source "$(dirname "$0")/env.sh"
cd "$REPO_ROOT"
"$REPO_ROOT/kernels/build_io.sh" || exit 1
for rep in $(seq 1 "${REPS:-2}"); do
  for cfg in ${CFGS:-none lru lfu clock adaptive}; do
    BENCH_CFG="$cfg" "$AURA_BIN" scripts/bench_cache.aura 2>&1 | grep -v '^\[aura'
  done
done
