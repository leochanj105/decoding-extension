// The real vocabulary, as XGrammar's TokenizerInfo. This is for candidate sources (and,
// later, native XGrammar for comparison); the checker never sees it and keeps
// compiling with an empty vocabulary.
//
// Scope: one tokenizer only, Qwen/Qwen2.5-Coder-0.5B-Instruct at Hugging Face revision
// ea3f2471cf1b1f0db85067f1ef93848e38e88c25, as fetched and checksummed by
// scripts/fetch_tokenizer.sh. This is not a general Hugging Face loader. It reads
// exactly what that model's files contain:
//   tokenizer.json          model.type "BPE", model.vocab, added_tokens
//   config.json             vocab_size
//   generation_config.json  eos_token_id (an integer or a list of integers)
// scripts/compare_with_python.sh checks the result against XGrammar's Python
// TokenizerInfo.from_huggingface for this tokenizer.
#pragma once

#include <xgrammar/xgrammar.h>

#include <string>
#include <vector>

namespace harness {

// The token strings of tokenizer.json, indexed by id, as they are stored (e.g. "Ġ="
// for " ="):model.vocab plus added_tokens, which is what tokenizer.get_vocab()
// returns. Requires ids 0..n-1, each exactly once. Nothing is decoded here.
std::vector<std::string> ReadEncodedVocab(const std::string& tokenizer_json_path);

// Build TokenizerInfo from the model directory, following what
// TokenizerInfo.from_huggingface does for this (fast, byte-level BPE) tokenizer:
//   - the token strings from tokenizer.json, padded with "" to config.json's
//     vocab_size (padding ids then count as special in XGrammar);
//   - vocab type and add_prefix_space from TokenizerInfo::DetectMetadataFromHF;
//   - stop token ids from generation_config.json's eos_token_id, each required to be
//     an integer in [0, vocab_size) before it is narrowed to int32_t.
// All token decoding (byte-level etc.) is done by TokenizerInfo itself.
// Throws std::runtime_error on anything outside that scope.
xgrammar::TokenizerInfo LoadQwenTokenizer(const std::string& dir);

}  // namespace harness
