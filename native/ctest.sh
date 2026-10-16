#!/usr/bin/env bash
# C-level randomized model test of the native data plane (every memtable rep,
# compaction style, cache policy; reopen; live switches) under ASan + UBSan,
# plus the deliberately wrong RKN_BUG_RESURRECT build, which must FAIL.
set -uo pipefail
source "$(dirname "$0")/../scripts/env.sh"
here="$REPO_ROOT/native"
inc="-I$SINK_DIR/kernels -I$AURA_LIB"
mkdir -p "$REPO_ROOT/build"
cc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -std=c11 $inc -o "$REPO_ROOT/build/rkn_ctest" "$here/rkn_ctest.c" || exit 1
"$REPO_ROOT/build/rkn_ctest" || { echo "FAIL native ctest"; exit 1; }
cc -O1 -g -std=c11 -DRKN_BUG_RESURRECT $inc -o "$REPO_ROOT/build/rkn_ctest_bug" "$here/rkn_ctest.c" || exit 1
if "$REPO_ROOT/build/rkn_ctest_bug" > /dev/null 2>&1; then echo "FAIL: the RKN_BUG_RESURRECT build passed the model test"; exit 1; fi
echo "ok   native ctest (and the wrong build is caught)"
