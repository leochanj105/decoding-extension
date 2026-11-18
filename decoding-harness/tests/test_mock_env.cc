// Tests for the mock environment ids in the parser. Every parser state carries entry_env
// and env. OnComplete receives the env of a completed [on_complete] rule and returns the
// env its waiting parent continues with. Each test's callback records (rule, text, env
// received) and applies a fixed integer transform, e.g. a: env + 1. Only failures are
// printed.
//
// Events are sorted before comparing: the order of completions at one input position is
// not part of the behavior under test.
#include "prefix_checker.h"

#include <algorithm>
#include <cstdio>
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

// One reported completion: rule name, matched text, env the callback received.
using Event = std::tuple<std::string, std::string, int32_t>;

// An env set by a completion reaches a later sibling, also through a nullable rule
// (ws is skipped when empty) and through XGrammar's helper rules for sp*.
static void TestPropagation() {
  const std::string grammar =
      "root ::= a ws b\n"
      "ws ::= sp*\n"
      "sp ::= \" \" | \"\\t\"\n"
      "a[on_complete] ::= \"x\"\n"
      "b[on_complete] ::= \"y\"\n";

  for (const std::string input : {"xy", "x \ty"}) {
    std::vector<Event> events;
    PrefixChecker checker(grammar, "root",
                          [&](const std::string& rule, const std::string& text, int64_t, int64_t, int32_t env) {
                            events.push_back({rule, text, env});
                            return rule == "a" ? env + 1 : env;
                          });

    // a receives 0 and returns 1; b, predicted after a, receives 1.
    CHECK(checker.TryAppend(input));
    std::vector<Event> expected = {{"a", "x", 0}, {"b", "y", 1}};
    CHECK(events == expected);
  }
}

// Used by two tests below: a and b match the same "x" but end with different envs.
static const std::string kAlternatives =
    "root ::= (a | b) c\n"
    "a[on_complete] ::= \"x\"\n"
    "b[on_complete] ::= \"x\"\n"
    "c[on_complete] ::= \"y\"\n";

// Two alternatives over the same bytes leave two parent states that differ only in env;
// they are kept apart, so c is reached once with each env.
static void TestAlternatives() {
  std::vector<Event> events;
  PrefixChecker checker(kAlternatives, "root",
                        [&](const std::string& rule, const std::string& text, int64_t, int64_t, int32_t env) {
                          events.push_back({rule, text, env});
                          return rule == "a" ? env + 1 : rule == "b" ? env + 2 : env;
                        });

  CHECK(checker.TryAppend("xy"));
  std::sort(events.begin(), events.end());
  std::vector<Event> expected = {{"a", "x", 0}, {"b", "x", 0}, {"c", "y", 1}, {"c", "y", 2}};
  CHECK(events == expected);
}

// A completed child resumes only the parent that predicted it with the child's entry
// env. Here two p occurrences wait (envs 1 and 2), each for its own c; each c adds 100.
// p is reported once per env (101, 102), not once per (child, parent) pair.
static void TestParentMatching() {
  const std::string grammar =
      "root ::= (a | b) p\n"
      "p[on_complete] ::= c\n"
      "a[on_complete] ::= \"x\"\n"
      "b[on_complete] ::= \"x\"\n"
      "c[on_complete] ::= \"y\"\n";
  std::vector<Event> events;
  PrefixChecker checker(grammar, "root",
                        [&](const std::string& rule, const std::string& text, int64_t, int64_t, int32_t env) {
                          events.push_back({rule, text, env});
                          if (rule == "a") return env + 1;
                          if (rule == "b") return env + 2;
                          if (rule == "c") return env + 100;
                          return env;
                        });

  CHECK(checker.TryAppend("xy"));
  std::sort(events.begin(), events.end());
  std::vector<Event> expected = {
      {"a", "x", 0}, {"b", "x", 0}, {"c", "y", 1}, {"c", "y", 2}, {"p", "y", 101}, {"p", "y", 102},
  };
  CHECK(events == expected);
}

// Probe and a refused TryAppend restore the parser states, envs included: the checker
// then equals a reference checker that never saw them.
static void TestRollback() {
  auto transform = [](const std::string& rule, int32_t env) {
    return rule == "a" ? env + 1 : rule == "b" ? env + 2 : env;
  };
  std::vector<Event> events;
  std::vector<Event> reference_events;
  PrefixChecker checker(kAlternatives, "root",
                        [&](const std::string& rule, const std::string& text, int64_t, int64_t, int32_t env) {
                          events.push_back({rule, text, env});
                          return transform(rule, env);
                        });
  PrefixChecker reference(kAlternatives, "root",
                          [&](const std::string& rule, const std::string& text, int64_t, int64_t, int32_t env) {
                            reference_events.push_back({rule, text, env});
                            return transform(rule, env);
                          });
  CHECK(checker.TryAppend("x"));
  CHECK(reference.TryAppend("x"));

  // After "x" the states carry the envs set by a and b, so DebugState shows them.
  CHECK(checker.DebugState().find("env=2") != std::string::npos);

  CHECK(checker.Probe("y"));
  CHECK(checker.DebugState() == reference.DebugState());

  // "yz": "y" is taken and completes c (an env transition), then 'z' is refused.
  int rejected_at = -1;
  CHECK(!checker.TryAppend("yz", &rejected_at));
  CHECK(rejected_at == 1);
  CHECK(checker.DebugState() == reference.DebugState());

  // Afterwards, "y" reports the same completions as on the reference checker.
  events.clear();
  reference_events.clear();
  CHECK(checker.TryAppend("y"));
  CHECK(reference.TryAppend("y"));
  std::sort(events.begin(), events.end());
  std::sort(reference_events.begin(), reference_events.end());
  std::vector<Event> expected = {{"c", "y", 1}, {"c", "y", 2}};
  CHECK(events == expected);
  CHECK(reference_events == expected);
  CHECK(checker.DebugState() == reference.DebugState());
}

int main() {
  TestPropagation();
  TestAlternatives();
  TestParentMatching();
  TestRollback();
  printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
