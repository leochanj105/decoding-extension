#!/usr/bin/env bash
# One-time setup, all into build/ and data/: Python env, the pinned dataset and model
# (sha256-checked), llama.cpp at a pinned release built for CPU, and the five tasks.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$HERE/build"
mkdir -p "$BUILD" "$HERE/data"

DATA_URL=https://huggingface.co/datasets/nuprl/MultiPL-E/resolve/28441b6024e71d4a1c1c0f6bf171c935cd5a43f2/mbpp-rs/test-00000-of-00001.parquet
DATA_SHA256=e1a8bad3f3dd05b465ab9545331c30a25a48779bb60a356c84a7954837ae7e4e
MODEL_URL=https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF/resolve/f86cb2c1fa58255f8052cc32aeede1b7482d4361/qwen2.5-coder-1.5b-instruct-q8_0.gguf
MODEL_SHA256=507de59046601282ba768a9789900e6ccf60ed93ddf346730b7c68eb0715bc47
LLAMA_TAG=b12385
LLAMA_COMMIT=d0764c5ee23a889680147c9342d2b9002cb87b82

# Download $1 to $2 unless it is already there with sha256 $3.
fetch() {
  if ! echo "$3  $2" | sha256sum -c --status 2>/dev/null; then
    curl -fL --retry 3 -o "$2" "$1"
    echo "$3  $2" | sha256sum -c
  fi
}

[ -x "$BUILD/venv/bin/python" ] || uv venv -q --python 3.10 "$BUILD/venv"
VIRTUAL_ENV="$BUILD/venv" uv pip install -q pyarrow==18.1.0

fetch "$DATA_URL" "$BUILD/mbpp-rs.parquet" "$DATA_SHA256"
fetch "$MODEL_URL" "$BUILD/qwen2.5-coder-1.5b-instruct-q8_0.gguf" "$MODEL_SHA256"

if [ ! -d "$BUILD/llama.cpp" ]; then
  git clone -q --depth 1 --branch "$LLAMA_TAG" https://github.com/ggml-org/llama.cpp "$BUILD/llama.cpp"
fi
[ "$(git -C "$BUILD/llama.cpp" rev-parse HEAD)" = "$LLAMA_COMMIT" ] || { echo "llama.cpp is not at $LLAMA_COMMIT"; exit 1; }
cmake -S "$BUILD/llama.cpp" -B "$BUILD/llama.cpp/build" -DCMAKE_BUILD_TYPE=Release \
  -DGGML_CUDA=OFF -DGGML_METAL=OFF -DGGML_VULKAN=OFF -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF > /dev/null
cmake --build "$BUILD/llama.cpp/build" --target llama-server -j "$(nproc)" > /dev/null

"$BUILD/venv/bin/python" -I "$HERE/extract_tasks.py" "$BUILD/mbpp-rs.parquet" "$HERE/data/tasks.jsonl"
