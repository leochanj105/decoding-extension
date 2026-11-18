#!/usr/bin/env bash
# Build libxgrammar.a from the existing ../xgrammar checkout, out of tree, into
# build/xgrammar. Refuses unless that checkout is exactly the pinned commit with no
# local changes: upstream 9aa840b plus our OnComplete extension (branch harness-base).
#
# Bindings, XGrammar's own tests and cpptrace are off. cmake/config.cmake sets them
# with a plain set(), which shadows -D, so the override is a config.cmake in the
# build directory.
set -euo pipefail

XG_COMMIT=e8f186da864908e6915617f8d14d8aeed0d7afdd
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XG="$(cd "$HERE/../xgrammar" && pwd)"
OUT="$HERE/build/xgrammar"

[ "$(git -C "$XG" rev-parse HEAD)" = "$XG_COMMIT" ] ||
  { echo "FAIL  $XG is not at $XG_COMMIT (git -C $XG checkout harness-base)"; exit 1; }
[ -z "$(git -C "$XG" status --porcelain)" ] ||
  { echo "FAIL  $XG has local changes"; exit 1; }

mkdir -p "$OUT"
cat > "$OUT/config.cmake" <<'CFG'
set(XGRAMMAR_BUILD_PYTHON_BINDINGS OFF)
set(XGRAMMAR_BUILD_CXX_TESTS OFF)
set(XGRAMMAR_ENABLE_COVERAGE OFF)
set(XGRAMMAR_ENABLE_CPPTRACE OFF)
set(XGRAMMAR_ENABLE_INTERNAL_CHECK OFF)
CFG
cmake -S "$XG" -B "$OUT" -DCMAKE_BUILD_TYPE=Release -G Ninja >/dev/null
cmake --build "$OUT" -j"$(nproc)" >/dev/null
echo "built $OUT/libxgrammar.a from $XG at $XG_COMMIT"
