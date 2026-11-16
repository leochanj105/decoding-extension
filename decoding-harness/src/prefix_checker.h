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
#include <functional>
#include <string>
#include <string_view>

namespace harness {

// Compile `ebnf` for parsing only: an empty vocabulary, one thread, no cache.
xgrammar::CompiledGrammar CompileWithoutMasks(
    const std::string& ebnf, const std::string& root = "root"
);

// Called on every completion of a rule marked [on_complete] in the grammar: the rule name,
// the matched bytes and their [begin, end) offsets in the accepted input. Observation
// only. Completions during Probe and refused TryAppend calls are reported too, although
// that input is then rolled back.
using OnComplete =
    std::function<void(const std::string& rule, const std::string& text, int64_t begin, int64_t end)>;

class PrefixChecker : public Checker {
 public:
  explicit PrefixChecker(const std::string& ebnf, const std::string& root = "root",
                         OnComplete on_complete = nullptr);

  // Feed `bytes` one at a time, stopping at the first byte the parser refuses.
  //   accepted: every byte is committed; returns true, and *rejected_at (if
  //             non-null) is -1.
  //   refused:  the committed state is exactly what it was before the call;
  //             returns false, and *rejected_at (if non-null) is the offset of
  //             the refused byte.
  // Empty input returns true and changes nothing.
  bool TryAppend(std::string_view bytes, int* rejected_at = nullptr) override;

  // Test whether TryAppend(bytes) would succeed, without committing anything.
  //   accepted: returns true, *rejected_at (if non-null) is -1.
  //   refused:  returns false, *rejected_at (if non-null) is the refused offset.
  // Either way the state afterwards, including CanStop and CommittedBytes, is exactly
  // what it was before the call. Empty input returns true and changes nothing.
  bool Probe(std::string_view bytes, int* rejected_at = nullptr) override;

  // May the program end here?
  bool CanStop() const override;

  int64_t CommittedBytes() const override { return committed_bytes_; }

  // The parser's own print of its state: the number of history rows and the live
  // states of the latest one. For tests.
  std::string DebugState() const;

 private:
  // Feed `bytes` into the parser one at a time. *rejected_at (if non-null) starts at
  // -1. On the first refused byte, roll back the bytes already taken, set
  // *rejected_at to its offset, and return false. On success the bytes
  // stay in the parser (not yet counted in committed_bytes_) and it returns true.
  bool Advance(std::string_view bytes, int* rejected_at);

  xgrammar::GrammarMatcher matcher_;
  int64_t committed_bytes_ = 0;
};

}  // namespace harness
