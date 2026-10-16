#!/usr/bin/env bash
# Run scripts/bench_adapt.aura for every static config and the adaptive
# controller, REPS times each (one process per run), appending to
# build/bench_adapt.jsonl; trajectories of the adaptive runs go to
# build/adapt-traj.jsonl.
set -uo pipefail
source "$(dirname "$0")/env.sh"
cd "$REPO_ROOT"
"$REPO_ROOT/kernels/build_io.sh" || exit 1
for rep in $(seq 1 "${REPS:-2}"); do
  for cfg in ${CFGS:-skiplist:leveled vector:tiered hash:leveled skiplist:tiered vector:leveled hash:tiered adaptive}; do
    BENCH_CFG="$cfg" "$AURA_BIN" scripts/bench_adapt.aura 2>&1 | grep -v '^\[aura'
  done
done
