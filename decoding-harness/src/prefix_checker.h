// Decides whether bytes can extend the program written so far. At this step only the
// grammar is enforced, by XGrammar's Earley parser reached through its public
// GrammarMatcher, with no token mask computed anywhere. Semantic constraints (types,
// scopes) are not checked yet.
//
// Two facts about upstream XGrammar make this possible without changing it:
//
//   - GrammarCompiler stops after optimising the grammar when the vocabulary is
//     empty (grammar_compiler.cc, MultiThreadCompileGrammar), so the per-position
//     token-mask precomputation never runs.
//   - GrammarMatcher::AcceptString advances byte by byte, stops at the first byte
//     the parser refuses, and on refusal pops exactly the rows it pushed. Nothing on
//     that path touches mask code.
//
// Preconditions, stated rather than checked: the grammar has no max_tokens/max_chars
// budget rules (their enforcement is driven by the previous mask call), and no
// element that names token ids (there is no vocabulary for them to name).
#pragma once

#include <xgrammar/xgrammar.h>

#include "checker.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace harness {

// Compile `ebnf` for parsing only: an empty vocabulary, one thread, no cache.
xgrammar::CompiledGrammar CompileWithoutMasks(
    const std::string& ebnf, const std::string& root = "root"
);

class PrefixChecker : public Checker {
 public:
  explicit PrefixChecker(const std::string& ebnf, const std::string& root = "root");

  // Feed `bytes` one at a time, stopping at the first byte the parser refuses.
  //   accepted: every byte is committed; returns true.
  //   refused:  the committed state is exactly what it was before the call;
  //             returns false, and *rejected_at (if non-null) is the offset of
  //             the refused byte.
  // Empty input returns true and changes nothing.
  bool TryAppend(std::string_view bytes, int* rejected_at = nullptr) override;

  // May the program end here?
  bool CanStop() const override;

  int64_t CommittedBytes() const override { return committed_bytes_; }

  // The parser's own print of its state: the number of history rows and the live
  // states of the latest one. For tests.
  std::string DebugState() const;

 private:
  xgrammar::GrammarMatcher matcher_;
  int64_t committed_bytes_ = 0;
};

}  // namespace harness
