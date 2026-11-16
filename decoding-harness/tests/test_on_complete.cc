// Tests for OnComplete: completions of rules marked [on_complete] (in the Rust grammar,
// variable_use) are reported with the rule name, matched text and [begin, end) offsets.
#include "prefix_checker.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using harness::PrefixChecker;

static int failures = 0;

static void Expect(bool ok, const std::string& what) {
  printf("%s  %s\n", ok ? "pass" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

struct Event {
  std::string rule;
  std::string text;
  int64_t begin;
  int64_t end;
  bool operator==(const Event& o) const {
    return rule == o.rule && text == o.text && begin == o.begin && end == o.end;
  }
};
using Events = std::vector<Event>;

static std::string Show(const Events& events) {
  std::string out;
  for (const auto& e : events) {
    out += " " + e.rule + " \"" + e.text + "\" [" + std::to_string(e.begin) + "," + std::to_string(e.end) + ")";
  }
  return "{" + out + " }";
}

static void ExpectEvents(const Events& actual, const Events& expected, const std::string& what) {
  bool ok = actual == expected;
  printf("%s  %s: %s\n", ok ? "pass" : "FAIL", what.c_str(), Show(actual).c_str());
  if (!ok) {
    printf("      expected: %s\n", Show(expected).c_str());
    ++failures;
  }
}

// A callback that appends every report to `events`.
static harness::OnComplete Into(Events* events) {
  return [events](const std::string& rule, const std::string& text, int64_t begin, int64_t end) {
    events->push_back({rule, text, begin, end});
  };
}

static bool SameState(const PrefixChecker& a, const PrefixChecker& b) {
  return a.DebugState() == b.DebugState() && a.CommittedBytes() == b.CommittedBytes() &&
         a.CanStop() == b.CanStop();
}

static bool Contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

static std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    fprintf(stderr, "cannot read %s\n", path.c_str());
    exit(2);
  }
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static const std::string kRust = ReadFile(HARNESS_SOURCE_DIR "/grammars/rust_fragment_v0.ebnf");

static const Events kOut = {
    {"variable_use", "o", 2, 3},
    {"variable_use", "ou", 2, 4},
    {"variable_use", "out", 2, 5},
};
static const Events kPut = {
    {"variable_use", "outp", 2, 6},
    {"variable_use", "outpu", 2, 7},
    {"variable_use", "output", 2, 8},
};

// 1. "{ out", then "put }".
static void TestOutThenPut() {
  Events events;
  PrefixChecker checker(kRust, "root", Into(&events));

  Expect(checker.TryAppend("{ out"), "\"{ out\" is accepted");
  ExpectEvents(events, kOut, "\"{ out\" reports");

  events.clear();
  Expect(checker.TryAppend("put }"), "then \"put }\" is accepted");
  ExpectEvents(events, kPut, "\"put }\" reports");
  Expect(checker.CanStop(), "the program may end after \"{ output }\"");
}

// 2. Probe "put }" after "{ out", then append it: the same events.
static void TestProbeThenAppend() {
  Events events;
  PrefixChecker checker(kRust, "root", Into(&events));
  checker.TryAppend("{ out");
  events.clear();

  Expect(checker.Probe("put }"), "Probe \"put }\" is accepted");
  Events probed = events;
  ExpectEvents(probed, kPut, "Probe \"put }\" reports");

  events.clear();
  Expect(checker.TryAppend("put }"), "TryAppend \"put }\" is accepted");
  ExpectEvents(events, probed, "TryAppend \"put }\" reports the same as the Probe");
}

// 3. A failed TryAppend restores the state; later input behaves like a reference checker.
static void TestFailedTryAppend() {
  Events events;
  Events reference_events;
  PrefixChecker checker(kRust, "root", Into(&events));
  PrefixChecker reference(kRust, "root", Into(&reference_events));
  checker.TryAppend("{ out");
  reference.TryAppend("{ out");

  int rejected_at = -1;
  Expect(!checker.TryAppend("pu&", &rejected_at), "TryAppend \"pu&\" is refused");
  Expect(rejected_at == 2, "  ...at the '&' (offset 2)");
  Expect(SameState(checker, reference), "  ...and the state equals the reference checker's");

  events.clear();
  reference_events.clear();
  Expect(checker.TryAppend("put }"), "then \"put }\" is accepted");
  Expect(reference.TryAppend("put }"), "  ...and by the reference checker");
  ExpectEvents(events, kPut, "\"put }\" reports");
  ExpectEvents(reference_events, kPut, "the reference checker reports");
  Expect(SameState(checker, reference), "  ...and the states are equal");
}

static std::string Optimized(const std::string& ebnf) {
  return harness::CompileWithoutMasks(ebnf).GetGrammar().ToString();
}

// 4. [on_complete] rules survive optimization, and their callbacks fire.
static void TestSurvival() {
  Expect(Contains(Optimized(kRust), "variable_use[on_complete] ::="),
         "the optimized Rust grammar still has variable_use[on_complete]");

  // Inlining: x refers to no other rule, so without the attribute it is inlined away.
  const std::string inlinable = "root ::= x \"c\"\nx[on_complete] ::= \"b\"\n";
  const std::string inlinable_plain = "root ::= x \"c\"\nx ::= \"b\"\n";
  Expect(!Contains(Optimized(inlinable_plain), "\nx ::="), "control: a plain x is inlined away");
  Expect(Contains(Optimized(inlinable), "\nx[on_complete] ::="), "x[on_complete] is kept");
  Events events;
  PrefixChecker inlined(inlinable, "root", Into(&events));
  Expect(inlined.TryAppend("bc"), "\"bc\" is accepted");
  ExpectEvents(events, {{"x", "b", 0, 1}}, "x reports");

  // Right recursion: w is the last element of v, so the shortcut would complete v's
  // parent straight from w and skip v's own completion.
  events.clear();
  PrefixChecker recursive("root ::= \"a\" v\nv[on_complete] ::= \"b\" w\nw ::= \"c\"\n", "root", Into(&events));
  Expect(recursive.TryAppend("abc"), "\"abc\" is accepted");
  ExpectEvents(events, {{"v", "bc", 1, 3}}, "v reports");

  // Initialization: the root's initial row is expanded when the matcher is built,
  // before the callback is set; child is the root's last element.
  events.clear();
  PrefixChecker initial("root[on_complete] ::= child\nchild[on_complete] ::= \"x\"\n", "root", Into(&events));
  Expect(initial.TryAppend("x"), "\"x\" is accepted");
  ExpectEvents(events, {{"child", "x", 0, 1}, {"root", "x", 0, 1}}, "child and root both report");
}

int main() {
  TestOutThenPut();
  TestProbeThenAppend();
  TestFailedTryAppend();
  TestSurvival();
  printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
