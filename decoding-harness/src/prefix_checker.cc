#include "prefix_checker.h"

#include <optional>
#include <vector>

namespace harness {

xgrammar::CompiledGrammar CompileWithoutMasks(const std::string& ebnf, const std::string& root) {
  xgrammar::GrammarCompiler compiler(
      xgrammar::TokenizerInfo(std::vector<std::string>{}),
      /*max_threads=*/1,
      /*cache_enabled=*/false
  );
  return compiler.CompileGrammar(ebnf, root);
}

PrefixChecker::PrefixChecker(const std::string& ebnf, const std::string& root)
    : matcher_(CompileWithoutMasks(ebnf, root), std::nullopt, /*terminate_without_stop_token=*/true) {}

bool PrefixChecker::TryAppend(std::string_view bytes, int* rejected_at) {
  // One AcceptString per byte, so a refusal knows its offset. Each accepted byte is
  // one entry in the matcher's rollback history, so undoing the i bytes already
  // taken is Rollback(i) -- the same PopLastStates a single AcceptString would do.
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (!matcher_.AcceptString(std::string(1, bytes[i]))) {
      if (i > 0) matcher_.Rollback(static_cast<int>(i));
      if (rejected_at != nullptr) *rejected_at = static_cast<int>(i);
      return false;
    }
  }
  committed_bytes_ += static_cast<int64_t>(bytes.size());
  return true;
}

bool PrefixChecker::CanStop() const { return matcher_.IsCompleted(); }

std::string PrefixChecker::DebugState() const { return matcher_._DebugPrintInternalState(); }

}  // namespace harness
