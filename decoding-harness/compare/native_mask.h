// Native XGrammar mask generation, for comparison with ExhaustiveMask. Kept out of
// src/ (and out of the no-mask checks): this code calls XGrammar's own mask path,
// with its normal settings -- a GrammarCompiler over the real vocabulary (default
// threads, cache on) and a GrammarMatcher with default options.
#pragma once

#include <xgrammar/xgrammar.h>

#include <cstdint>
#include <string>
#include <vector>

namespace harness {

class NativeMasker {
 public:
  NativeMasker(const xgrammar::TokenizerInfo& info, const std::string& ebnf);

  bool AcceptToken(int32_t token_id) { return matcher_.AcceptToken(token_id); }

  // Native mask generation only: FillNextTokenBitmask into the buffer allocated by
  // the constructor. No conversion here, so this call can be timed on its own.
  void FillBitmask() { matcher_.FillNextTokenBitmask(&tensor_); }

  // XGrammar's bitmask as last filled: bit (id % 32) of word (id / 32), 1 = allowed.
  const std::vector<int32_t>& Bitmask() const { return bitmask_; }

 private:
  xgrammar::GrammarMatcher matcher_;
  std::vector<int32_t> bitmask_;
  int64_t shape_[1];
  DLTensor tensor_;
};

// Unpack an XGrammar bitmask into one entry per token id (true = allowed).
std::vector<bool> BitmaskToMask(const std::vector<int32_t>& bitmask, int vocab_size);

}  // namespace harness
