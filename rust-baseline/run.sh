#!/usr/bin/env bash
# Start one llama-server (CPU, model loaded once), generate the five completions,
# stop the server, then compile and run the programs. Results go to results/.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$HERE/build"
OUT="$HERE/results"
PY="$BUILD/venv/bin/python -I"
PORT=8091
THREADS=8  # physical cores
mkdir -p "$OUT"

SERVER=("$BUILD/llama.cpp/build/bin/llama-server" -m "$BUILD/qwen2.5-coder-1.5b-instruct-q8_0.gguf"
        --host 127.0.0.1 --port "$PORT" -t "$THREADS" -c 4096 -np 1 -ngl 0 --jinja)

# Environment record: revisions, versions, the exact server command.
{
  echo "llama.cpp: $(git -C "$BUILD/llama.cpp" describe --tags) $(git -C "$BUILD/llama.cpp" rev-parse HEAD)"
  echo "rustc: $(rustc -vV | tr '\n' ' ')"
  echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-)"
  echo "server: ${SERVER[*]}"
} > "$OUT/environment.txt"

"${SERVER[@]}" > "$OUT/server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT
until curl -sf "http://127.0.0.1:$PORT/health" > /dev/null; do
  kill -0 $SERVER_PID 2>/dev/null || { echo "server exited; see $OUT/server.log"; exit 1; }
  sleep 1
done

$PY "$HERE/generate.py" "$HERE/data/tasks.jsonl" "http://127.0.0.1:$PORT" "$OUT/generations.jsonl"
kill $SERVER_PID; wait $SERVER_PID 2>/dev/null || true

$PY "$HERE/evaluate.py" "$HERE/data/tasks.jsonl" "$OUT/generations.jsonl" "$OUT"
