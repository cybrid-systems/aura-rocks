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
for t in "${files[@]}"; do
  s=$(date +%s)
  out=$("$AURA_BIN" "$t" 2>&1); rc=$?
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
