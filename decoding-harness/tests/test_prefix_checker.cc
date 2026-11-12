// Tests for PrefixChecker, one guarantee per function.
#include "prefix_checker.h"

#include <picojson.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using harness::PrefixChecker;

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
static PrefixChecker After(const std::string& prefix) { return With(kGrammar, prefix); }

// Same parser state (as XGrammar prints it), same byte count, same CanStop.
static bool SameState(const PrefixChecker& a, const PrefixChecker& b) {
  return a.DebugState() == b.DebugState() && a.CommittedBytes() == b.CommittedBytes() &&
         a.CanStop() == b.CanStop();
}

// Number of entries in the compiled grammar's adaptive token mask cache.
static size_t MaskCacheEntries(const xgrammar::CompiledGrammar& g) {
  picojson::value v;
  picojson::parse(v, g.SerializeJSON());
  return v.get("adaptive_token_mask_cache").get<picojson::array>().size();
}

static void TestNoMasks() {
  check(MaskCacheEntries(harness::CompileWithoutMasks(kGrammar)) == 0,
        "no token masks are built");

  // Control: with a vocabulary, the same grammar does build masks, so the check above
  // can fail. The gdb check turns this off (HARNESS_SKIP_CONTROL) for its checker run.
  if (std::getenv("HARNESS_SKIP_CONTROL")) return;
  xgrammar::GrammarCompiler with_vocab(xgrammar::TokenizerInfo({"l", "et", " ", "x"}), 1, false);
  check(MaskCacheEntries(with_vocab.CompileGrammar(kGrammar, "root")) > 0,
        "control: with a vocabulary, masks are built");
}

static void TestAccept() {
  PrefixChecker c(kGrammar);
  int at = 42;
  bool ok = c.TryAppend("let x") && !c.CanStop() && c.TryAppend(" = 1;", &at);
  check(ok && c.CanStop(), "a valid program is accepted, and CanStop is true only at its end");
  check(SameState(c, After("let x = 1;")), "accepting in two pieces equals accepting at once");
  check(at == -1, "rejected_at is -1 on success");
}

static void TestPartialThenReject() {
  PrefixChecker c = After("let "), before = After("let ");
  int at = -1;
  check(!c.TryAppend("x9 =", &at) && at == 1, "\"x9 =\": 'x' accepted, '9' refused at offset 1");
  check(SameState(c, before), "  ...and the state is as before the call");
}

static void TestCompleteThenReject() {
  PrefixChecker c = After("let x = 1"), before = After("let x = 1");
  int at = -1;
  check(!c.TryAppend(";;", &at) && at == 1, "\";;\": first ';' completes the program, second refused");
  check(SameState(c, before) && !c.CanStop(), "  ...and the state, including CanStop, is as before");
}

static void TestContinueAfterRollback() {
  PrefixChecker c = After("let ");
  c.TryAppend("x9 =");
  check(c.TryAppend("x = 1;") && SameState(c, After("let x = 1;")),
        "after a refusal, a valid continuation is accepted as if the refusal never happened");
}

static void TestEmptyInput() {
  PrefixChecker c = After("let "), before = After("let ");
  int at = 42;
  check(c.TryAppend("", &at) && at == -1 && SameState(c, before),
        "empty input is accepted and changes nothing");
}

// Probe: the same answer TryAppend would give, but the state never changes.

// `c` has been probed; a checker given only `prefix` has not. Both must answer each
// append the same way: result, rejected_at, and state afterwards.
static bool BehavesAsUnprobed(PrefixChecker& c, const char* grammar, const std::string& prefix,
                              const std::vector<std::string>& appends) {
  PrefixChecker unprobed = With(grammar, prefix);
  bool same = SameState(c, unprobed);
  for (const auto& a : appends) {
    int at_c = 42, at_u = 42;
    same = same && c.TryAppend(a, &at_c) == unprobed.TryAppend(a, &at_u) && at_c == at_u &&
           SameState(c, unprobed);
  }
  return same;
}

static void TestProbeAccepts() {
  PrefixChecker c = After("let ");
  int at = 42;
  check(c.Probe("x = 1", &at) && at == -1, "probe \"x = 1\" after \"let \": accepted, rejected_at = -1");
  check(BehavesAsUnprobed(c, kGrammar, "let ", {"9", "x = 1", ";;", ";"}),
        "  ...then appends (refused and accepted) behave as on an unprobed checker");
}

static void TestProbePartialThenReject() {
  PrefixChecker c = After("let ");
  int at = 42;
  check(!c.Probe("x9 =", &at) && at == 1, "probe \"x9 =\": 'x' accepted, '9' refused at offset 1");
  check(BehavesAsUnprobed(c, kGrammar, "let ", {"x9", "x", " = 1;"}),
        "  ...then appends behave as on an unprobed checker");
}

static void TestProbeTemporarilyCompletes() {
  PrefixChecker c = After("let x = 1");
  int at = 42;
  check(c.Probe(";", &at) && at == -1 && !c.CanStop(),
        "probe \";\" would complete the program, but CanStop stays false afterwards");
  check(!c.Probe(";;", &at) && at == 1, "probe \";;\": completes, then refused at offset 1");
  check(BehavesAsUnprobed(c, kGrammar, "let x = 1", {"2", ";", ";"}),
        "  ...then appends behave as on an unprobed checker");
}

static void TestProbeSplitUtf8() {
  // "\xC3\xA9" is "é". A lone lead byte is a valid prefix; the string cannot close
  // until the character is complete.
  PrefixChecker c = With(kString, "s = \"");
  int a1 = 42, a2 = 42, a3 = 42;
  bool ok = c.Probe("\xC3", &a1) && !c.Probe("\xC3\"", &a2) && c.Probe("\xC3\xA9\";", &a3) && !c.CanStop();
  check(ok && a1 == -1 && a2 == 1 && a3 == -1,
        "probes of a split \"é\": lead byte accepted, quote after it refused, full string completes");
  check(BehavesAsUnprobed(c, kString, "s = \"", {"\xC3", "\"", "\xA9", "\";"}),
        "  ...then appends, one byte of \"é\" at a time, behave as on an unprobed checker");

  PrefixChecker mid = With(kString, "s = \"\xC3");  // half of "é" committed
  ok = mid.Probe("\xA9", &a1) && !mid.Probe("\"", &a2) && mid.Probe("\xA9\";", &a3) && !mid.CanStop();
  check(ok && a1 == -1 && a2 == 0 && a3 == -1, "probes between the two bytes of \"é\"");
  check(BehavesAsUnprobed(mid, kString, "s = \"\xC3", {"\"", "\xA9", "\";"}),
        "  ...then appends behave as on an unprobed checker");
}

static void TestRepeatedProbes() {
  PrefixChecker c = After("let ");
  bool same = true;
  for (int i = 0; i < 3; ++i) {
    int ok_at = 42, bad_at = 42;
    same = same && c.Probe("x = 1", &ok_at) && ok_at == -1 && !c.Probe("x9", &bad_at) && bad_at == 1;
  }
  check(same && BehavesAsUnprobed(c, kGrammar, "let ", {"x = 1;"}),
        "probing the same bytes again gives the same answer each time, and changes nothing");
}

static void TestProbeEmpty() {
  PrefixChecker c = After("let ");
  int at = 42;
  check(c.Probe("", &at) && at == -1 && BehavesAsUnprobed(c, kGrammar, "let ", {"x = 1;"}),
        "probe of empty input: accepted, rejected_at = -1, nothing changes");
}

int main() {
  TestNoMasks();
  TestAccept();
  TestPartialThenReject();
  TestCompleteThenReject();
  TestContinueAfterRollback();
  TestEmptyInput();
  TestProbeAccepts();
  TestProbePartialThenReject();
  TestProbeTemporarilyCompletes();
  TestProbeSplitUtf8();
  TestRepeatedProbes();
  TestProbeEmpty();
  printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
