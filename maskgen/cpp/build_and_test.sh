#!/usr/bin/env bash
# Build xgrammar's C++ library and run our tests against it.
#
# Bindings are off, so no torch or tvm-ffi is needed. Note that
# xgrammar/cmake/config.cmake sets XGRAMMAR_BUILD_PYTHON_BINDINGS with a plain
# set(), which shadows -D on the command line, so the override has to be a
# config.cmake placed in the build directory.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/../.."
XG="$ROOT/xgrammar"
BUILD="${BUILD_DIR:-$HERE/build}"
VOCAB="${VOCAB:-$BUILD/vocab.txt}"

mkdir -p "$BUILD"
cat > "$BUILD/config.cmake" <<'CFG'
set(XGRAMMAR_BUILD_PYTHON_BINDINGS OFF)
set(XGRAMMAR_BUILD_CXX_TESTS OFF)
set(XGRAMMAR_ENABLE_COVERAGE OFF)
set(XGRAMMAR_ENABLE_CPPTRACE OFF)
set(XGRAMMAR_ENABLE_INTERNAL_CHECK OFF)
CFG

cmake -S "$XG" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -G Ninja >/dev/null
cmake --build "$BUILD" -j"$(nproc)"

if [ ! -f "$VOCAB" ]; then
  echo "dumping the tokenizer vocabulary to $VOCAB"
  "$ROOT/profiling/venv/bin/python" - "$VOCAB" <<'PY'
import sys
from tokenizers import Tokenizer
out = sys.argv[1]
tk = Tokenizer.from_file(__import__("os").path.join(
    __import__("os").path.dirname(out), "..", "..", "..", "profiling", "tok.json"))
vocab = tk.get_vocab()
arr = [""] * (max(vocab.values()) + 1)
for text, i in vocab.items():
    arr[i] = text
with open(out, "w", encoding="utf-8") as f:
    f.write(f"{len(arr)}\n")
    for s in arr:
        f.write(s + "\n")
PY
fi

for src in "$HERE"/test_*.cc; do
  exe="$BUILD/$(basename "${src%.cc}")"
  g++ -O2 -std=c++17 \
    -I"$XG/include" -I"$XG/cpp" \
    -I"$XG/3rdparty/dlpack/include" -I"$XG/3rdparty/picojson" \
    "$src" -o "$exe" -L"$BUILD" -lxgrammar -lpthread 2>&1 | grep -v lto-wrapper || true
  echo "--- $(basename "$exe") ---"
  "$exe" "$VOCAB"
done
