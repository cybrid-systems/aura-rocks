#!/usr/bin/env bash
# Run tests/*.aura (or the files given). A file fails on a FAIL line, a crash,
# a non-zero exit, an `error:` line, zero PASS lines, or a RUN-ONCE marker not
# printed exactly once (same rules as aura-kernel-sink).
set -uo pipefail
source "$(dirname "$0")/env.sh"
cd "$REPO_ROOT"
"$REPO_ROOT/kernels/build_io.sh" || exit 1
status=0
files=("$@")
[ ${#files[@]} -eq 0 ] && files=("$REPO_ROOT"/tests/*.aura)
# tests that take a config run once per memtable-rep:compaction-style pair (ROCKS_TEST_REP)
runs=()
for t in "${files[@]}"; do
  if grep -q 'ROCKS_TEST_REP' "$t"; then
    for r in ${ROCKS_TEST_REPS:-skiplist:leveled vector:leveled hash:leveled skiplist:tiered vector:tiered hash:tiered}; do runs+=("$t@$r"); done
  else
    runs+=("$t@")
  fi
done
for run in "${runs[@]}"; do
  t="${run%@*}"; rep="${run##*@}"
  s=$(date +%s)
  out=$(ROCKS_TEST_REP="$rep" "$AURA_BIN" "$t" 2>&1); rc=$?
  [ -n "$rep" ] && t="$t[$rep]"
  pass=$(grep -c '^PASS' <<<"$out" || true)
  once=$(grep -c '^RUN-ONCE' <<<"$out" || true)
  secs=$(( $(date +%s) - s ))
  if [ $rc -ne 0 ] || [ "$pass" -eq 0 ] || [ "$once" -ne 1 ] || grep -qE '^FAIL|AURA CRASH|^error:|load_module_file:|<error>' <<<"$out"; then
    echo "FAIL ${t#$REPO_ROOT/} (rc=$rc, $pass checks, RUN-ONCE x$once, ${secs}s)"
    echo "$out" | grep -E '^FAIL|CRASH|^error|load_module_file|<error>' | head -20
    status=1
  else
    echo "ok   ${t#$REPO_ROOT/} ($pass checks, ${secs}s)"
  fi
  [ -n "${VERBOSE:-}" ] && echo "$out"
done
exit $status
