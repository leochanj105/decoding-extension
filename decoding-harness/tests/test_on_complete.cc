// Tests for OnComplete: every completion of a rule marked [on_complete] (in the Rust
// grammar, variable_use) is reported with the rule name, the matched text and its
// [begin, end) byte offsets. Only failures are printed.
#include "prefix_checker.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using harness::PrefixChecker;

static int failures = 0;

#define CHECK(condition)                                        \
  do {                                                          \
    if (!(condition)) {                                         \
      printf("FAIL line %d: %s\n", __LINE__, #condition);       \
      ++failures;                                               \
    }                                                           \
  } while (0)

// One reported completion: rule name, matched text, begin, end.
using Event = std::tuple<std::string, std::string, int64_t, int64_t>;

// A callback that records every completion and leaves the environment unchanged.
static harness::OnComplete RecordInto(std::vector<Event>* events) {
  return [events](const std::string& rule, const std::string& text, int64_t begin, int64_t end, int32_t env) {
    events->push_back({rule, text, begin, end});
    return env;
  };
}

static std::string ReadRustGrammar() {
  std::ifstream in(HARNESS_SOURCE_DIR "/grammars/rust_fragment_v0.ebnf");
  if (!in) {
    printf("cannot read the Rust grammar\n");
    exit(2);
  }
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

static const std::string kRust = ReadRustGrammar();

// After "{ out", the name may still grow or may end; both continuations are tried.
static void TestOutThenPut() {
  std::vector<Event> events;
  PrefixChecker checker(kRust, "root", RecordInto(&events));

  // Each byte of "out" completes variable_use once more, always starting at offset 2.
  CHECK(checker.TryAppend("{ out"));
  std::vector<Event> expected = {
      {"variable_use", "o", 2, 3},
      {"variable_use", "ou", 2, 4},
      {"variable_use", "out", 2, 5},
  };
  CHECK(events == expected);

  // "put }" grows the same name to "output", and the program may then end.
  events.clear();
  CHECK(checker.TryAppend("put }"));
  expected = {
      {"variable_use", "outp", 2, 6},
      {"variable_use", "outpu", 2, 7},
      {"variable_use", "output", 2, 8},
  };
  CHECK(events == expected);
  CHECK(checker.CanStop());

  // Alternatively, " }" ends the name right away: accepted, and nothing new is reported.
  std::vector<Event> other_events;
  PrefixChecker other(kRust, "root", RecordInto(&other_events));
  CHECK(other.TryAppend("{ out"));
  other_events.clear();
  CHECK(other.TryAppend(" }"));
  CHECK(other_events.empty());
  CHECK(other.CanStop());
}

// Probing "put }" reports the same completions as appending it afterwards.
static void TestProbeThenAppend() {
  std::vector<Event> events;
  PrefixChecker checker(kRust, "root", RecordInto(&events));
  CHECK(checker.TryAppend("{ out"));

  events.clear();
  CHECK(checker.Probe("put }"));
  std::vector<Event> probed = events;
  std::vector<Event> expected = {
      {"variable_use", "outp", 2, 6},
      {"variable_use", "outpu", 2, 7},
      {"variable_use", "output", 2, 8},
  };
  CHECK(probed == expected);

  events.clear();
  CHECK(checker.TryAppend("put }"));
  CHECK(events == probed);
}

// A refused TryAppend leaves the checker exactly as a reference checker that never saw
// it, and later input reports the same completions with the same offsets.
static void TestFailedTryAppend() {
  std::vector<Event> events;
  std::vector<Event> reference_events;
  PrefixChecker checker(kRust, "root", RecordInto(&events));
  PrefixChecker reference(kRust, "root", RecordInto(&reference_events));
  CHECK(checker.TryAppend("{ out"));
  CHECK(reference.TryAppend("{ out"));

  // "pu&" is refused at '&' (offset 2), after "pu" was taken; both bytes are rolled back.
  int rejected_at = -1;
  CHECK(!checker.TryAppend("pu&", &rejected_at));
  CHECK(rejected_at == 2);
  CHECK(checker.DebugState() == reference.DebugState());

  events.clear();
  reference_events.clear();
  CHECK(checker.TryAppend("put }"));
  CHECK(reference.TryAppend("put }"));
  std::vector<Event> expected = {
      {"variable_use", "outp", 2, 6},
      {"variable_use", "outpu", 2, 7},
      {"variable_use", "output", 2, 8},
  };
  CHECK(events == expected);
  CHECK(reference_events == expected);
  CHECK(checker.DebugState() == reference.DebugState());
}

// [on_complete] rules survive XGrammar's optimizations, and their callbacks fire in the
// three places where a completion could otherwise be removed or skipped.
static void TestSurvival() {
  // The rule is still in the grammar XGrammar actually parses with.
  std::string optimized = harness::CompileWithoutMasks(kRust).GetGrammar().ToString();
  CHECK(optimized.find("variable_use[on_complete] ::=") != std::string::npos);

  // Inlining: x refers to no other rule, so a plain x is inlined away (control), but
  // x[on_complete] is kept and reports its completion.
  std::string plain = harness::CompileWithoutMasks("root ::= x \"c\"\nx ::= \"b\"\n").GetGrammar().ToString();
  CHECK(plain.find("\nx ::=") == std::string::npos);
  const std::string inlinable = "root ::= x \"c\"\nx[on_complete] ::= \"b\"\n";
  optimized = harness::CompileWithoutMasks(inlinable).GetGrammar().ToString();
  CHECK(optimized.find("\nx[on_complete] ::=") != std::string::npos);
  std::vector<Event> events;
  PrefixChecker inlined(inlinable, "root", RecordInto(&events));
  CHECK(inlined.TryAppend("bc"));
  std::vector<Event> expected = {{"x", "b", 0, 1}};
  CHECK(events == expected);

  // Right recursion: w is the last element of v, so the shortcut would complete v's
  // parent straight from w and skip v's own completion.
  events.clear();
  PrefixChecker recursive("root ::= \"a\" v\nv[on_complete] ::= \"b\" w\nw ::= \"c\"\n", "root", RecordInto(&events));
  CHECK(recursive.TryAppend("abc"));
  expected = {{"v", "bc", 1, 3}};
  CHECK(events == expected);

  // Initialization: child is the root's last element, and the root's first row is
  // expanded when the matcher is built, before the callback is set.
  events.clear();
  PrefixChecker initial("root[on_complete] ::= child\nchild[on_complete] ::= \"x\"\n", "root", RecordInto(&events));
  CHECK(initial.TryAppend("x"));
  expected = {{"child", "x", 0, 1}, {"root", "x", 0, 1}};
  CHECK(events == expected);
}

int main() {
  TestOutThenPut();
  TestProbeThenAppend();
  TestFailedTryAppend();
  TestSurvival();
  printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
