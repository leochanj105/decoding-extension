// What the greedy decoder needs from a checker, and nothing more. The decoder never
// sees how a checker decides; PrefixChecker is one implementation.
#pragma once

#include <cstdint>
#include <string_view>

namespace harness {

class Checker {
 public:
  virtual ~Checker() = default;

  // Commit `bytes` if they extend the program; otherwise change nothing and return
  // false, with *rejected_at (if non-null) the offset of the refused byte.
  virtual bool TryAppend(std::string_view bytes, int* rejected_at = nullptr) = 0;

  // May the program end here?
  virtual bool CanStop() const = 0;

  // Bytes committed so far; 0 for a fresh checker.
  virtual int64_t CommittedBytes() const = 0;
};

}  // namespace harness
