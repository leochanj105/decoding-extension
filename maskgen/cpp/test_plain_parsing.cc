// Ordinary grammars must still parse, since we changed shared parser paths.
//
// Three changes touch grammars that use none of our features: the completion sites
// now copy the parent state instead of listing its fields, a hook was added at
// completion, and AcceptString now buffers each byte before advancing rather than
// after the loop. This covers nesting, alternation and repetition, with rejects as
// well as accepts, which is what those changes could plausibly break.
//
// Deliberately NOT xgrammar's own suite: of its 73 tests only two drive the parser,
// and both are JSON whitespace regressions. These few are the targeted replacement.
#include <xgrammar/xgrammar.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace xgrammar;

static int failures = 0;
static void check(bool ok, const std::string& what) {
  printf("%s  %s\n", ok ? "pass" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

static bool Whole(const CompiledGrammar& g, const std::string& input) {
  GrammarMatcher m(g, std::nullopt, /*terminate_without_stop_token=*/true);
  return m.AcceptString(input) && m.IsTerminated();
}

int main(int, char**) {
  GrammarCompiler compiler(TokenizerInfo(std::vector<std::string>{}), 1, false);

  // nested rules, so a child completing has to advance its parent
  auto nested = compiler.CompileGrammar(
      "root  ::= \"(\" inner \")\"\n"
      "inner ::= \"a\" | \"(\" inner \")\"\n", "root");
  check(Whole(nested, "(a)"), "nested: (a)");
  check(Whole(nested, "(((a)))"), "nested: (((a)))");
  check(!Whole(nested, "(a"), "nested rejects an unclosed (a");
  check(!Whole(nested, "(b)"), "nested rejects (b)");

  // alternation where both branches share a prefix
  auto shared = compiler.CompileGrammar(
      "root ::= \"ab\" | \"ac\" | \"abc\"\n", "root");
  check(Whole(shared, "ab") && Whole(shared, "ac") && Whole(shared, "abc"),
        "shared prefixes: ab, ac, abc all accepted");
  check(!Whole(shared, "a") && !Whole(shared, "abd"),
        "shared prefixes: a and abd rejected");

  // repetition, since two completion sites carry a repeat count
  auto repeated = compiler.CompileGrammar("root ::= \"x\"{2,4}\n", "root");
  check(!Whole(repeated, "x"), "repeat{2,4} rejects one x");
  check(Whole(repeated, "xx") && Whole(repeated, "xxxx"), "repeat{2,4} accepts two and four");
  check(!Whole(repeated, "xxxxx"), "repeat{2,4} rejects five");

  // right recursion, whose completion path is deliberately different
  auto list = compiler.CompileGrammar(
      "root ::= item root | item\n"
      "item ::= \"i\"\n", "root");
  check(Whole(list, "i") && Whole(list, "iiii"), "right recursion: i and iiii");
  check(!Whole(list, ""), "right recursion rejects the empty string");

  // byte-at-a-time versus all-at-once must agree, which is what the AcceptString
  // byte-ordering change could break
  {
    GrammarMatcher one(nested, std::nullopt, true);
    bool all_at_once = one.AcceptString("(((a)))");
    GrammarMatcher many(nested, std::nullopt, true);
    bool one_by_one = true;
    for (char c : std::string("(((a)))")) {
      one_by_one = one_by_one && many.AcceptString(std::string(1, c));
    }
    check(all_at_once && one_by_one && one.IsTerminated() && many.IsTerminated(),
          "one call and one-call-per-byte give the same result");
  }

  printf("\n%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
