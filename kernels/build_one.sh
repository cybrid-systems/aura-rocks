#!/usr/bin/env bash
# aura-kernel-sink's background compile (lib/bg.aura) runs
# $SINK_ROOT/kernels/build_one.sh <src.c> <out.so>. SINK_ROOT is this repo
# (generated C and .so land under build/), so delegate to the submodule's
# script, which compiles against its kernels/sink_abi.h + Aura's
# lib/aura_value_abi.h.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
exec "${SINK_DIR:-$root/third_party/aura-kernel-sink}/kernels/build_one.sh" "$@"
