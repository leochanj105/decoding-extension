// Tests for PrefixChecker, one guarantee per function.
#include "prefix_checker.h"

#include <picojson.h>

#include <cstdio>
#include <cstdlib>
#include <string>

using harness::PrefixChecker;

static int failures = 0;
static void check(bool ok, const char* what) {
  printf("%s  %s\n", ok ? "pass" : "FAIL", what);
  if (!ok) ++failures;
}

static const char* kGrammar = R"(root ::= "let " [a-z]+ " = " [0-9]+ ";")";

static PrefixChecker After(const std::string& prefix) {
  PrefixChecker c(kGrammar);
  c.TryAppend(prefix);
  return c;
}

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
  check(at == 42, "rejected_at is not written on success");
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
  check(c.TryAppend("", &at) && at == 42 && SameState(c, before),
        "empty input is accepted and changes nothing");
}

int main() {
  TestNoMasks();
  TestAccept();
  TestPartialThenReject();
  TestCompleteThenReject();
  TestContinueAfterRollback();
  TestEmptyInput();
  printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
