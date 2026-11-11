#!/usr/bin/env bash
# Build the harness and its tests, and link them against the libxgrammar.a that
# build_xgrammar.sh produced. Uses only XGrammar's public headers.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XG="$(cd "$HERE/../xgrammar" && pwd)"
BUILD="$HERE/build"
LIB="$BUILD/xgrammar/libxgrammar.a"

[ -f "$LIB" ] || { echo "FAIL  $LIB not found; run scripts/build_xgrammar.sh first"; exit 1; }

CXX=(g++ -O2 -g -std=c++17 -Wall -Wextra
     -I"$XG/include" -I"$XG/3rdparty/dlpack/include" -I"$XG/3rdparty/picojson" -I"$HERE/src")
"${CXX[@]}" -c "$HERE/src/prefix_checker.cc" -o "$BUILD/prefix_checker.o"
for src in "$HERE"/tests/test_*.cc; do
  "${CXX[@]}" "$src" "$BUILD/prefix_checker.o" -o "$BUILD/$(basename "${src%.cc}")" "$LIB" -lpthread
done
echo "built the harness in $BUILD, linked against $LIB"
