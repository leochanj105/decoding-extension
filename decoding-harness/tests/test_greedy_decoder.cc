// Tests for GreedyDecode with a scripted source. Each test edits one entry of the
// valid script below and checks what changes.
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "candidate_source.h"
#include "greedy_decoder.h"
#include "prefix_checker.h"

using namespace harness;
using Script = std::map<std::string, std::vector<Candidate>>;
using Ids = std::vector<int32_t>;

static int failures = 0;
static void check(bool ok, const char* what) {
  printf("%s  %s\n", ok ? "pass" : "FAIL", what);
  if (!ok) ++failures;
}

static const char* kGrammar = R"(root ::= "let " [a-z]+ " = " [0-9]+ ";")";
static const int32_t kEos = 0;

// A complete run, one candidate per step: "let " "x" " = " "1" ";" EOS -- 6 steps.
static Script Valid() {
  return {
      {"", {{1, "let ", 1.0}}},
      {"let ", {{2, "x", 1.0}}},
      {"let x", {{3, " = ", 1.0}}},
      {"let x = ", {{4, "1", 1.0}}},
      {"let x = 1", {{5, ";", 1.0}}},
      {"let x = 1;", {{kEos, "", 1.0}}},
  };
}

static DecodeResult Run(ScriptedSource& source, int max_steps = 10) {
  PrefixChecker checker(kGrammar);  // the grammar is built here, outside the loop
  return GreedyDecode(checker, source, {kEos, max_steps});
}

static bool Throws(const std::function<void()>& f) {
  try {
    f();
  } catch (const std::invalid_argument&) {
    return true;
  }
  return false;
}

static void TestPartialRejectionThenAcceptance() {
  // At "let ": "x9" takes 'x' and is refused at '9'. " = " would be accepted only if
  // that 'x' had stayed committed. "x" is valid.
  Script s = Valid();
  s["let "] = {{6, "x9", 0.9}, {7, " = ", 0.7}, {2, "x", 0.5}};
  ScriptedSource source(s);
  DecodeResult r = Run(source);
  check(r.outcome == Outcome::kStopped && r.bytes == "let x = 1;" && r.token_ids == Ids{1, 2, 3, 4, 5},
        "a partly accepted candidate is refused, and the next valid one is committed");

  check(source.seen_bytes ==
            std::vector<std::string>{"", "let ", "let x", "let x = ", "let x = 1", "let x = 1;"},
        "  the source saw only committed bytes");
  check(source.seen_token_ids == std::vector<Ids>{{}, {1}, {1, 2}, {1, 2, 3}, {1, 2, 3, 4}, {1, 2, 3, 4, 5}},
        "  the source saw only committed token ids");
}

static void TestScoreOrderAndTies() {
  Script s = Valid();
  s["let "] = {{8, "z", 0.2}, {2, "x", 0.8}, {9, "q", 0.5}};  // not listed in score order
  ScriptedSource by_score(s);
  check(Run(by_score).token_ids == Ids{1, 2, 3, 4, 5}, "the highest score wins, whatever the list order");

  bool lower_id_wins = true;
  for (const auto& tied : {std::vector<Candidate>{{2, "x", 0.8}, {20, "x", 0.8}},
                           std::vector<Candidate>{{20, "x", 0.8}, {2, "x", 0.8}}}) {
    s["let "] = tied;
    ScriptedSource source(s);
    lower_id_wins = lower_id_wins && Run(source).token_ids == Ids{1, 2, 3, 4, 5};
  }
  check(lower_id_wins, "equal scores: the lower token id wins, in either list order");
}

static void TestPrematureEos() {
  Script s = Valid();
  s["let x"] = {{kEos, "", 0.9}, {3, " = ", 0.5}};
  ScriptedSource source(s);
  DecodeResult r = Run(source);
  check(r.outcome == Outcome::kStopped && r.bytes == "let x = 1;" && r.token_ids == Ids{1, 2, 3, 4, 5} &&
            r.steps == 6,
        "EOS before the program can stop is refused; the next candidate is committed");
}

static void TestExhaustion() {
  Script s = Valid();
  s["let x"] = {{20, ";", 0.9}, {21, "", 0.8}, {kEos, "", 0.5}};  // all refused
  ScriptedSource source(s);
  DecodeResult r = Run(source);
  check(r.outcome == Outcome::kExhausted && r.bytes == "let x" && r.token_ids == Ids{1, 2} && r.steps == 3,
        "no candidate accepted (invalid bytes, empty bytes, EOS too early): exhausted");

  s = Valid();
  s.erase("let x = ");
  ScriptedSource empty(s);
  r = Run(empty);
  check(r.outcome == Outcome::kExhausted && r.bytes == "let x = " && r.steps == 4,
        "an empty candidate list: exhausted");
}

static void TestStepLimits() {
  ScriptedSource zero(Valid());
  DecodeResult r = Run(zero, 0);
  check(r.outcome == Outcome::kStepLimit && r.steps == 0 && zero.seen_bytes.empty(),
        "max_steps = 0: step limit, and the source is never called");

  ScriptedSource exact(Valid());
  r = Run(exact, 6);
  check(r.outcome == Outcome::kStopped && r.steps == 6, "max_steps = 6, exactly the steps needed: stopped");

  ScriptedSource one_short(Valid());
  r = Run(one_short, 5);
  check(r.outcome == Outcome::kStepLimit && r.bytes == "let x = 1;" && r.steps == 5,
        "max_steps = 5: step limit, although the program is complete");
}

static void TestInvalidInput() {
  ScriptedSource source(Valid());
  check(Throws([&] { Run(source, -1); }), "max_steps < 0 is rejected");

  check(Throws([&] {
          PrefixChecker used(kGrammar);
          used.TryAppend("let ");
          GreedyDecode(used, source, {kEos, 10});
        }),
        "a checker that is not fresh is rejected");

  Script s = Valid();
  s["let "] = {{2, "x", 0.9}, {2, "y", 0.5}};
  ScriptedSource duplicate(s);
  check(Throws([&] { Run(duplicate); }), "a duplicate token id is rejected");

  s["let "] = {{2, "x", std::nan("")}};
  ScriptedSource nan_score(s);
  check(Throws([&] { Run(nan_score); }), "a NaN score is rejected");
}

// The whole loop on the grammar "ab", every kind of refusal once.
static void TestIntegrationAb() {
  ScriptedSource source({
      {"", {{10, "aX", 0.9}, {1, "a", 0.5}}},                    // "aX" refused at 'X'
      {"a", {{kEos, "", 0.9}, {11, "c", 0.8}, {2, "b", 0.5}}},   // EOS too early, "c" refused
      {"ab", {{kEos, "", 1.0}}},                                  // EOS accepted
  });
  PrefixChecker checker(R"(root ::= "ab")");
  DecodeResult r = GreedyDecode(checker, source, {kEos, 10});

  check(r.outcome == Outcome::kStopped && r.bytes == "ab" && r.token_ids == Ids{1, 2} && r.steps == 3,
        "\"ab\": stopped after 3 steps with tokens {1, 2}");
  check(source.seen_bytes == std::vector<std::string>{"", "a", "ab"} &&
            source.seen_token_ids == std::vector<Ids>{{}, {1}, {1, 2}},
        "\"ab\": the source was called with (\"\", {}), (\"a\", {1}), (\"ab\", {1, 2})");
}

// The same list at every step, all scores equal, so the lowest id the grammar accepts
// wins. Whether the run finishes depends only on that order.
static void TestFixedVocab() {
  //                        0      1     2      3       4     5
  FixedVocabSource finishes({" = ", ";", "1", "<eos>", "x", "let "});
  PrefixChecker c1(kGrammar);
  DecodeResult r = GreedyDecode(c1, finishes, {/*eos_token_id=*/3, 10});
  check(r.outcome == Outcome::kStopped && r.bytes == "let x = 1;" && r.token_ids == Ids{5, 4, 0, 2, 1} &&
            r.steps == 6,
        "fixed list: the grammar alone picks \"let x = 1;\", then EOS");

  //                     0     1      2     3      4       5
  FixedVocabSource stuck({"x", " = ", "1", ";", "<eos>", "let "});
  PrefixChecker c2(kGrammar);
  r = GreedyDecode(c2, stuck, {/*eos_token_id=*/4, 5});
  check(r.outcome == Outcome::kStepLimit && r.bytes == "let xxxx",
        "fixed list: \"x\" (lowest id) stays valid in the name forever, so the step limit is hit");
}

int main() {
  TestIntegrationAb();
  TestFixedVocab();
  TestPartialRejectionThenAcceptance();
  TestScoreOrderAndTies();
  TestPrematureEos();
  TestExhaustion();
  TestStepLimits();
  TestInvalidInput();
  printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
