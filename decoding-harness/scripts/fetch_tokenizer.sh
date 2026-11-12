#!/usr/bin/env bash
# Download the tokenizer files of Qwen/Qwen2.5-Coder-0.5B-Instruct at the pinned
# Hugging Face revision into data/, and verify them. The harness reads tokenizer.json,
# config.json and generation_config.json; tokenizer_config.json is read only by
# transformers, in scripts/compare_with_python.sh. The checksums were taken from the
# files at REVISION, whose git blob ids match the ones Hugging Face lists for it.
set -euo pipefail

REPO=Qwen/Qwen2.5-Coder-0.5B-Instruct
REVISION=ea3f2471cf1b1f0db85067f1ef93848e38e88c25
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="$HERE/data/qwen2.5-coder-0.5b-instruct"

declare -A SHA256=(
  [tokenizer.json]=c0382117ea329cdf097041132f6d735924b697924d6f6fc3945713e96ce87539
  [config.json]=b1e58593cd31852f7da5c2fc31ddf6135b9c066c0fd9177a4bbe95717083adff
  [generation_config.json]=fdaccbcb02f3e1e7914ccb0f69ebe899071ffd27cf825166d823163e156870f2
  [tokenizer_config.json]=959e7f1d9a1b7641a6d6ce05ca97b75c7894fcb66cbe5a040406458fb1128ee4
)

mkdir -p "$DIR"
for f in "${!SHA256[@]}"; do
  [ -f "$DIR/$f" ] || curl -sfL -o "$DIR/$f" "https://huggingface.co/$REPO/resolve/$REVISION/$f"
  echo "${SHA256[$f]}  $DIR/$f" | sha256sum --check --quiet ||
    { echo "FAIL  $f does not match the pinned checksum"; exit 1; }
done
echo "tokenizer $REPO at $REVISION in $DIR"
