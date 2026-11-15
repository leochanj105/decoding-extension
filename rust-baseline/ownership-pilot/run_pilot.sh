#!/usr/bin/env bash
# Ownership pilot, generation only. One model at a time: download it if missing
# (sha256-checked), start one llama-server (CPU, 8 threads; model loaded once), run
# both prompts (1 greedy + 4 sampled each), stop the server. Writes outputs.jsonl,
# outputs.md and one server log per model into this directory.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$HERE/../build"
PY="$BUILD/venv/bin/python -I"
PORT=8091

# id  repo  revision  file  sha256
MODELS=(
  "qwen2.5-coder-1.5b-q8_0 Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF f86cb2c1fa58255f8052cc32aeede1b7482d4361 qwen2.5-coder-1.5b-instruct-q8_0.gguf 507de59046601282ba768a9789900e6ccf60ed93ddf346730b7c68eb0715bc47"
  "qwen2.5-coder-0.5b-q8_0 Qwen/Qwen2.5-Coder-0.5B-Instruct-GGUF ebb2015119c907b064c512bf053e945850b5875f qwen2.5-coder-0.5b-instruct-q8_0.gguf e1a77721fa97d412f121878223eec81fb4ae6f271e18f922d746711f67b344d1"
)

rm -f "$HERE/outputs.jsonl"
for m in "${MODELS[@]}"; do
  read -r ID REPO REV FILE SHA <<< "$m"
  GGUF="$BUILD/$FILE"
  if ! echo "$SHA  $GGUF" | sha256sum -c --status 2>/dev/null; then
    curl -fL --retry 3 -o "$GGUF" "https://huggingface.co/$REPO/resolve/$REV/$FILE"
    echo "$SHA  $GGUF" | sha256sum -c
  fi

  "$BUILD/llama.cpp/build/bin/llama-server" -m "$GGUF" --host 127.0.0.1 --port "$PORT" \
      -t 8 -c 4096 -np 1 -ngl 0 --jinja > "$HERE/$ID.server.log" 2>&1 &
  SERVER_PID=$!
  trap 'kill $SERVER_PID 2>/dev/null || true' EXIT
  until curl -sf "http://127.0.0.1:$PORT/health" > /dev/null; do
    kill -0 $SERVER_PID 2>/dev/null || { echo "server exited; see $ID.server.log"; exit 1; }
    sleep 1
  done
  $PY "$HERE/pilot_generate.py" "http://127.0.0.1:$PORT" "$ID" "$REPO" "$REV" "$FILE" "$HERE/outputs.jsonl" \
      "$HERE/bitwriter_new.prompt.txt" "$HERE/hmac_sha1.prompt.txt"
  kill $SERVER_PID; wait $SERVER_PID 2>/dev/null || true
done

$PY "$HERE/pilot_outputs_md.py" "$HERE/outputs.jsonl" "$HERE/outputs.md"
echo "llama.cpp $(git -C "$BUILD/llama.cpp" describe --tags) $(git -C "$BUILD/llama.cpp" rev-parse HEAD)"
