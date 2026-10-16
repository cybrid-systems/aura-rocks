#!/usr/bin/env bash
# Build kernels/rocks_io.c -> build/kernels/librocks_io.so (FFI file helpers).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$root/build/kernels"
so="$root/build/kernels/librocks_io.so"
if [ ! -f "$so" ] || [ "$root/kernels/rocks_io.c" -nt "$so" ]; then
  "${CC:-cc}" -O2 -std=c11 -Wall -Wextra -Werror -shared -fPIC -fvisibility=hidden -o "$so" "$root/kernels/rocks_io.c"
  echo "built build/kernels/librocks_io.so"
fi
