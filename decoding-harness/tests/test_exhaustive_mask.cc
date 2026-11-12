// Tests for ExhaustiveMask: on a tiny vocabulary with masks written out by hand, and
// on the real Qwen vocabulary against an independent reference and the greedy decoder.
#include <picojson.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

#include "candidate_source.h"
#include "exhaustive_mask.h"
#include "greedy_decoder.h"
#include "prefix_checker.h"
#include "vocabulary.h"

using namespace harness;
using Mask = std::vector<bool>;
using Ids = std::vector<int32_t>;

static int failures = 0;
static void check(bool ok, const char* what) {
  printf("%s  %s\n", ok ? "pass" : "FAIL", what);
  if (!ok) ++failures;
}

static const char* kGrammar = R"(root ::= "let " [a-z]+ " = " [0-9]+ ";")";
static const char* kString = R"(root ::= "s = \"" [^"]* "\";")";

static PrefixChecker With(const char* grammar, const std::string& prefix) {
  PrefixChecker c(grammar);
  c.TryAppend(prefix);
  return c;
}

// Same parser state (as XGrammar prints it), same byte count, same CanStop.
static bool SameState(const PrefixChecker& a, const PrefixChecker& b) {
  return a.DebugState() == b.DebugState() && a.CommittedBytes() == b.CommittedBytes() &&
         a.CanStop() == b.CanStop();
}

// The real vocabulary, loaded on first use (inside main; see test_vocabulary.cc).
static const xgrammar::TokenizerInfo& Info() {
  static const xgrammar::TokenizerInfo info =
      LoadQwenTokenizer(std::string(HARNESS_DATA_DIR) + "/qwen2.5-coder-0.5b-instruct");
  return info;
}
static const std::vector<std::string>& Decoded() { return Info().GetDecodedVocab(); }
static const Ids& Stops() { return Info().GetStopTokenIds(); }

static int32_t IdWithBytes(const std::string& bytes) {
  auto it = std::find(Decoded().begin(), Decoded().end(), bytes);
  return it == Decoded().end() ? -1 : static_cast<int32_t>(it - Decoded().begin());
}

// --- tiny vocabulary -------------------------------------------------------------

//                                    0       1     2      3    4    5   6    7     8
static const std::vector<std::string> kTiny = {"let ", "x", " = ", "1", ";", "", "x", "x9", "le"};
static const Ids kTinyEos = {6};  // an EOS token whose bytes "x" would be valid after "let "

static Mask TinyMask(const std::string& prefix) {
  PrefixChecker c = With(kGrammar, prefix);
  return ExhaustiveMask(c, kTiny, kTinyEos);
}

static void TestKnownMasks() {
  //                       0      1      2      3      4      5      6      7      8
  check(TinyMask("") == Mask{true, false, false, false, false, false, false, false, true},
        "at \"\": \"let \" and \"le\"");
  check(TinyMask("let ") == Mask{true, true, false, false, false, false, false, false, true},
        "at \"let \": \"x\", and \"let \", \"le\" (the name may be \"let\"); \"x9\" is denied at '9',"
        " and the EOS id 6 is denied although its bytes \"x\" would fit");
  check(TinyMask("let x = 1") == Mask{false, false, false, true, true, false, false, false, false},
        "at \"let x = 1\": \"1\" and \";\"");
  check(TinyMask("let x = 1;") == Mask{false, false, false, false, false, false, true, false, false},
        "at \"let x = 1;\": only EOS");
}

static void TestInvalidEos() {
  PrefixChecker c(kGrammar);
  bool too_big = false, negative = false;
  try {
    ExhaustiveMask(c, kTiny, {9});
  } catch (const std::invalid_argument&) {
    too_big = true;
  }
  try {
    ExhaustiveMask(c, kTiny, {-1});
  } catch (const std::invalid_argument&) {
    negative = true;
  }
  check(too_big && negative, "an eos id outside the vocabulary (9, -1) is rejected");
}

// --- real vocabulary: full mask against an independent reference -----------------

// Every prefix of the language of kGrammar, written as a regex. No XGrammar code.
static const std::regex kLanguagePrefix(
    "|l|le|let|let |let [a-z]+|let [a-z]+ |let [a-z]+ =|let [a-z]+ = |let [a-z]+ = [0-9]+"
    "|let [a-z]+ = [0-9]+;");
static const std::regex kLanguage("let [a-z]+ = [0-9]+;");

static bool Matches(const std::string& s, const std::regex& re) {
  return std::regex_match(s.begin(), s.end(), re);
}

static void TestFullMaskAgainstReference() {
  const std::vector<std::string> prefixes = {"",          "l",          "let",         "let ",
                                             "let a",     "let ab ",    "let ab =",    "let ab = ",
                                             "let ab = 1", "let ab = 12", "let ab = 12;"};
  std::vector<bool> is_stop(Decoded().size(), false);
  for (int32_t id : Stops()) is_stop[id] = true;

  bool all_same = true, none_empty = true;
  for (const auto& p : prefixes) {
    PrefixChecker c = With(kGrammar, p);
    Mask mask = ExhaustiveMask(c, Decoded(), Stops());
    bool complete = Matches(p, kLanguage);
    int allowed = 0, mismatches = 0;
    for (size_t id = 0; id < Decoded().size(); ++id) {
      const std::string& b = Decoded()[id];
      bool expected = is_stop[id] ? complete : !b.empty() && Matches(p + b, kLanguagePrefix);
      allowed += mask[id];
      if (mask[id] != expected && ++mismatches <= 3)
        printf("      at \"%s\": id %zu bytes \"%s\": mask %d, reference %d\n", p.c_str(), id, b.c_str(),
               static_cast<int>(mask[id]), static_cast<int>(expected));
    }
    printf("      at \"%s\": %d allowed, %d mismatches\n", p.c_str(), allowed, mismatches);
    all_same = all_same && mismatches == 0;
    none_empty = none_empty && allowed > 0;
    if (complete) {
      Ids allowed_ids;
      for (size_t id = 0; id < mask.size(); ++id)
        if (mask[id]) allowed_ids.push_back(static_cast<int32_t>(id));
      Ids stops = Stops();
      std::sort(stops.begin(), stops.end());
      check(allowed_ids == stops, "at the complete program, exactly the two stop ids are allowed");
    }
  }
  check(all_same, "all 151936 entries equal the regex reference at each of 11 prefixes of \"let ab = 12;\"");
  check(none_empty, "  every one of those prefixes allows at least one token");
}

static void TestRepeatedCalls() {
  PrefixChecker c = With(kGrammar, "let x");
  Mask first = ExhaustiveMask(c, Decoded(), Stops());
  bool same = ExhaustiveMask(c, Decoded(), Stops()) == first && ExhaustiveMask(c, Decoded(), Stops()) == first;
  check(same, "three calls at the same prefix give the same mask");
}

static void TestReversedOrder() {
  // Reverse the vocabulary (id i becomes n-1-i), so every token is probed in the
  // opposite order; reversed back, the mask must be the same.
  const size_t n = Decoded().size();
  std::vector<std::string> reversed(Decoded().rbegin(), Decoded().rend());
  Ids reversed_stops;
  for (int32_t id : Stops()) reversed_stops.push_back(static_cast<int32_t>(n - 1 - id));

  bool same = true;
  for (const std::string p : {"let x = 1", "let x = 1;"}) {
    PrefixChecker c1 = With(kGrammar, p), c2 = With(kGrammar, p);
    Mask forward = ExhaustiveMask(c1, Decoded(), Stops());
    Mask backward = ExhaustiveMask(c2, reversed, reversed_stops);
    same = same && Mask(backward.rbegin(), backward.rend()) == forward;
  }
  check(same, "probing in reverse id order gives the same mask (at \"let x = 1\" and \"let x = 1;\")");
}

// --- real vocabulary: the checker is unchanged -----------------------------------

// `c` has had a mask computed; a checker given only `prefix` has not. Both must answer
// each append the same way: result, rejected_at, and state afterwards.
static bool BehavesAsUntouched(PrefixChecker& c, const char* grammar, const std::string& prefix,
                               const std::vector<std::string>& appends) {
  PrefixChecker untouched = With(grammar, prefix);
  bool same = SameState(c, untouched);
  for (const auto& a : appends) {
    int at_c = 42, at_u = 42;
    same = same && c.TryAppend(a, &at_c) == untouched.TryAppend(a, &at_u) && at_c == at_u &&
           SameState(c, untouched);
  }
  return same;
}

static void TestStateUnchanged() {
  struct Case {
    const char* grammar;
    std::string prefix;
    std::vector<std::string> appends;
  };
  const std::vector<Case> cases = {
      {kGrammar, "let x", {"9", "x", " = 1;"}},
      {kGrammar, "let x = 1", {";;", "2", ";"}},  // one token from complete
      {kGrammar, "let x = 1;", {";", "x"}},       // complete: CanStop is true
      {kString, "s = \"\xC3", {"\"", "\xA9", "\";"}},  // half of "é" committed
  };
  bool same = true;
  for (const auto& k : cases) {
    PrefixChecker c = With(k.grammar, k.prefix);
    ExhaustiveMask(c, Decoded(), Stops());
    same = same && BehavesAsUntouched(c, k.grammar, k.prefix, k.appends);
  }
  check(same, "after a mask, appends behave as on an untouched checker (4 prefixes, incl. complete, split UTF-8)");

  PrefixChecker mid = With(kString, "s = \"\xC3");
  Mask mask = ExhaustiveMask(mid, Decoded(), Stops());
  int32_t a9 = IdWithBytes("\xA9"), quote = IdWithBytes("\"");
  check(a9 >= 0 && quote >= 0 && mask[a9] && !mask[quote],
        "  between the two bytes of \"é\": the token 0xA9 is allowed, the token \"\\\"\" is denied");
}

// --- real vocabulary: same choices as the greedy decoder -------------------------

// A few distinct values, so ties are common; depends on the token and the step.
static double HashScore(uint64_t seed, int32_t id, size_t committed) {
  uint64_t x = seed * 0x9E3779B97F4A7C15ULL ^ static_cast<uint64_t>(id) * 0xBF58476D1CE4E5B9ULL ^
               static_cast<uint64_t>(committed) * 0x94D049BB133111EBULL;
  x ^= x >> 31;
  x *= 0xD6E9F8CC1F4E5B37ULL;
  x ^= x >> 29;
  return static_cast<double>(x % 4);
}

class HashScoredSource : public CandidateSource {
 public:
  explicit HashScoredSource(uint64_t seed) : seed_(seed) {}
  std::vector<Candidate> Propose(const std::string& bytes, const Ids&) override {
    std::vector<Candidate> all;
    all.reserve(Decoded().size());
    for (size_t id = 0; id < Decoded().size(); ++id)
      all.push_back({static_cast<int32_t>(id), Decoded()[id], HashScore(seed_, id, bytes.size())});
    return all;
  }

 private:
  uint64_t seed_;
};

// Greedy decoding from the mask: each step, the allowed id with the highest score,
// the lower id on a tie.
static DecodeResult DecodeFromMasks(PrefixChecker& c, uint64_t seed, int max_steps) {
  DecodeResult r{Outcome::kStepLimit, "", {}, 0};
  while (r.steps < max_steps) {
    Mask mask = ExhaustiveMask(c, Decoded(), Stops());
    ++r.steps;
    int32_t best = -1;
    for (size_t id = 0; id < mask.size(); ++id) {
      if (mask[id] && (best < 0 || HashScore(seed, id, r.bytes.size()) > HashScore(seed, best, r.bytes.size())))
        best = static_cast<int32_t>(id);
    }
    if (best < 0) {
      r.outcome = Outcome::kExhausted;
      return r;
    }
    if (std::find(Stops().begin(), Stops().end(), best) != Stops().end()) {
      r.outcome = Outcome::kStopped;
      return r;
    }
    if (!c.TryAppend(Decoded()[best])) throw std::logic_error("an allowed token was refused");
    r.bytes += Decoded()[best];
    r.token_ids.push_back(best);
  }
  return r;
}

static void TestMatchesGreedyDecoder() {
  bool same = true;
  for (const char* grammar : {R"(root ::= "ab")", kGrammar}) {
    for (uint64_t seed : {1, 2}) {
      PrefixChecker c1(grammar), c2(grammar);
      HashScoredSource source(seed);
      DecodeResult a = GreedyDecode(c1, source, {Stops(), 12});
      DecodeResult b = DecodeFromMasks(c2, seed, 12);
      printf("      seed %d: outcome %d, %d steps, \"%s\"\n", static_cast<int>(seed), static_cast<int>(a.outcome),
             a.steps, a.bytes.c_str());
      same = same && a.outcome == b.outcome && a.token_ids == b.token_ids && a.bytes == b.bytes &&
             a.steps == b.steps;
    }
  }
  check(same, "highest-scored allowed token = GreedyDecode's choice (2 grammars x 2 seeds)");
}

// --- control for the gdb mask check ----------------------------------------------

// With a vocabulary, XGrammar's compiler builds masks, so the gdb breakpoints have
// something to hit. The gdb check turns this off (HARNESS_SKIP_CONTROL).
static void Control() {
  if (std::getenv("HARNESS_SKIP_CONTROL")) return;
  xgrammar::GrammarCompiler with_vocab(xgrammar::TokenizerInfo({"l", "et", " ", "x"}), 1, false);
  picojson::value v;
  picojson::parse(v, with_vocab.CompileGrammar(kGrammar, "root").SerializeJSON());
  check(!v.get("adaptive_token_mask_cache").get<picojson::array>().empty(),
        "control: XGrammar with a vocabulary builds masks");
}

int main() {
  Control();
  TestKnownMasks();
  TestInvalidEos();
  TestFullMaskAgainstReference();
  TestRepeatedCalls();
  TestReversedOrder();
  TestStateUnchanged();
  TestMatchesGreedyDecoder();
  printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
