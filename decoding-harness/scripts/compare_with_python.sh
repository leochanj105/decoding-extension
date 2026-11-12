#!/usr/bin/env bash
# Compare the harness's tokenizer loader with XGrammar's official Python loader
# (TokenizerInfo.from_huggingface over transformers' AutoTokenizer), for the pinned
# Qwen tokenizer: every id's decoded bytes, stop ids, special ids, size and metadata.
#
# Needs network the first time: it creates build/pyenv with the versions in
# scripts/python-requirements.txt and installs XGrammar's Python package from
# ../xgrammar (the same pinned commit build_xgrammar.sh checks), building it in
# build/pyxgrammar. Run ./build.sh first.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XG="$(cd "$HERE/../xgrammar" && pwd)"
BUILD="$HERE/build"
DIR="$HERE/data/qwen2.5-coder-0.5b-instruct"
PY="$BUILD/pyenv/bin/python"

XG_COMMIT=$(sed -n 's/^XG_COMMIT=//p' "$HERE/scripts/build_xgrammar.sh")
[ "$(git -C "$XG" rev-parse HEAD)" = "$XG_COMMIT" ] && [ -z "$(git -C "$XG" status --porcelain)" ] ||
  { echo "FAIL  ../xgrammar is not clean at $XG_COMMIT"; exit 1; }
[ -x "$BUILD/dump_tokenizer_info" ] || { echo "FAIL  run ./build.sh first"; exit 1; }

if [ ! -x "$PY" ]; then
  uv venv -q --python 3.10 "$BUILD/pyenv"
  VIRTUAL_ENV="$BUILD/pyenv" uv pip install -q --index-url https://download.pytorch.org/whl/cpu \
    "$(grep '^torch==' "$HERE/scripts/python-requirements.txt")"
  VIRTUAL_ENV="$BUILD/pyenv" uv pip install -q -r "$HERE/scripts/python-requirements.txt"
fi
if [ ! -f "$BUILD/pyxgrammar.commit" ] || [ "$(cat "$BUILD/pyxgrammar.commit")" != "$XG_COMMIT" ]; then
  VIRTUAL_ENV="$BUILD/pyenv" uv pip install -q --reinstall --no-deps --no-build-isolation \
    -C build-dir="$BUILD/pyxgrammar" "$XG"
  echo "$XG_COMMIT" > "$BUILD/pyxgrammar.commit"
fi

"$BUILD/dump_tokenizer_info" "$DIR" > "$BUILD/tokenizer_info.harness.txt"
"$PY" -I -W ignore "$HERE/scripts/dump_tokenizer_info.py" "$DIR" > "$BUILD/tokenizer_info.python.txt"

echo "harness: $(head -5 "$BUILD/tokenizer_info.harness.txt" | cut -c1-60 | tr '\n' '|')"
echo "python:  $(head -5 "$BUILD/tokenizer_info.python.txt" | cut -c1-60 | tr '\n' '|')"
echo "lines:   $(wc -l < "$BUILD/tokenizer_info.harness.txt") vs $(wc -l < "$BUILD/tokenizer_info.python.txt")"
if cmp "$BUILD/tokenizer_info.harness.txt" "$BUILD/tokenizer_info.python.txt"; then
  echo "pass  the harness loader and XGrammar's Python loader give identical TokenizerInfo"
else
  diff "$BUILD/tokenizer_info.harness.txt" "$BUILD/tokenizer_info.python.txt" | head -20
  echo "FAIL  the two loaders differ"; exit 1
fi
