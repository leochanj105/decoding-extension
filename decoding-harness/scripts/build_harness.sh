#!/usr/bin/env bash
# Build the harness, its tests and tools/, and link them against the libxgrammar.a that
# build_xgrammar.sh produced. Uses only XGrammar's public headers.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XG="$(cd "$HERE/../xgrammar" && pwd)"
BUILD="$HERE/build"
LIB="$BUILD/xgrammar/libxgrammar.a"

[ -f "$LIB" ] || { echo "FAIL  $LIB not found; run scripts/build_xgrammar.sh first"; exit 1; }

CXX=(g++ -O2 -g -std=c++17 -Wall -Wextra
     -I"$XG/include" -I"$XG/3rdparty/dlpack/include" -I"$XG/3rdparty/picojson" -I"$HERE/src"
     -DHARNESS_DATA_DIR="\"$HERE/data\"")
objs=()
for src in "$HERE"/src/*.cc; do
  obj="$BUILD/$(basename "${src%.cc}").o"
  "${CXX[@]}" -c "$src" -o "$obj"
  objs+=("$obj")
done
for src in "$HERE"/tests/test_*.cc "$HERE"/tools/*.cc; do
  "${CXX[@]}" "$src" "${objs[@]}" -o "$BUILD/$(basename "${src%.cc}")" "$LIB" -lpthread
done

# compare/: native XGrammar mask generation, for comparison only. Built into
# build/compare/, so it is never among the build/*.o that the no-mask checks scan.
mkdir -p "$BUILD/compare"
cmp_objs=()
for src in "$HERE"/compare/*.cc; do
  case "$(basename "$src")" in test_* | bench_*) continue ;; esac
  obj="$BUILD/compare/$(basename "${src%.cc}").o"
  "${CXX[@]}" -I"$HERE/compare" -c "$src" -o "$obj"
  cmp_objs+=("$obj")
done
for src in "$HERE"/compare/test_*.cc "$HERE"/compare/bench_*.cc; do
  "${CXX[@]}" -I"$HERE/compare" "$src" "${cmp_objs[@]}" "${objs[@]}" -o "$BUILD/compare/$(basename "${src%.cc}")" \
    "$LIB" -lpthread
done
echo "built the harness in $BUILD, linked against $LIB"
