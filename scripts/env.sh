# shellcheck shell=bash
# Shared environment for aura-rocks (Aura Soft face).
#   AURA_BIN   aura binary (required unless `aura` is on PATH)
#   AURA_LIB   Aura lib dir (default: <aura checkout>/lib, from AURA_BIN)
#   SINK_DIR   aura-kernel-sink checkout (default: third_party/aura-kernel-sink)
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AURA_BIN="${AURA_BIN:-$(command -v aura || true)}"
if [ -z "$AURA_BIN" ] || [ ! -x "$AURA_BIN" ]; then
  echo "error: set AURA_BIN to an Aura build (e.g. ~/aura/build-rel/aura)" >&2
  exit 2
fi
AURA_LIB="${AURA_LIB:-$(cd "$(dirname "$AURA_BIN")/../lib" 2>/dev/null && pwd)}"
SINK_DIR="${SINK_DIR:-$REPO_ROOT/third_party/aura-kernel-sink}"
if [ ! -f "$SINK_DIR/lib/vkgen.aura" ]; then
  echo "error: aura-kernel-sink not found at $SINK_DIR (git submodule update --init)" >&2
  exit 2
fi
export AURA_BIN AURA_LIB SINK_DIR
export AURA_PATH="${AURA_LIB}:${REPO_ROOT}/lib:${SINK_DIR}/lib"
export AURA_SANDBOX="${AURA_SANDBOX:-off}"
export AURA_PIPELINE_STRICT="${AURA_PIPELINE_STRICT:-0}"
export ROCKS_ROOT="$REPO_ROOT"
# aura-kernel-sink writes generated C / .so under SINK_ROOT (build/gen-bg, build/kernels)
export SINK_ROOT="$REPO_ROOT"
