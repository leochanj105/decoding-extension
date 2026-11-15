// Syntax tests for grammars/rust_fragment_v0.ebnf. Each test gives a prefix (what the
// model has written so far), commits it with TryAppend, computes the real next-token
// mask with ExhaustiveMask over the Qwen vocabulary, and checks that some legal tokens
// are allowed and some illegal ones denied. Tokens are named by their text; EOS stands
// for the stop tokens.
//
// Syntax only: undeclared names are accepted here. Known lexical limitation
// (temporary, not the intended final behavior): identifiers may be keywords.
#include "exhaustive_mask.h"
#include "prefix_checker.h"
#include "vocabulary.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int failures = 0;

static std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) fprintf(stderr, "cannot read %s\n", path.c_str()), exit(2);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static const std::string kGrammar = ReadFile(HARNESS_SOURCE_DIR "/grammars/rust_fragment_v0.ebnf");
static const std::string EOS = "<EOS>";  // not a token text: means the stop tokens

static const xgrammar::TokenizerInfo& Info() {
  static const xgrammar::TokenizerInfo info =
      harness::LoadQwenTokenizer(std::string(HARNESS_DATA_DIR) + "/qwen2.5-coder-0.5b-instruct");
  return info;
}

// The mask entry for a token text (exactly one id must have that text), or for EOS.
static bool Allowed(const std::vector<bool>& mask, const std::string& token) {
  if (token == EOS) return mask[Info().GetStopTokenIds().at(0)];
  const auto& vocab = Info().GetDecodedVocab();
  if (std::count(vocab.begin(), vocab.end(), token) != 1)
    fprintf(stderr, "test bug: \"%s\" is not exactly one token\n", token.c_str()), exit(2);
  return mask[std::find(vocab.begin(), vocab.end(), token) - vocab.begin()];
}

static std::string Show(const std::string& s) {
  std::string out;
  for (char c : s) out += c == '\n' ? "\\n" : c == '\r' ? "\\r" : c == '\t' ? "\\t" : std::string(1, c);
  return "\"" + out + "\"";
}

static void Check(const std::string& prefix, const std::vector<std::string>& allowed,
                  const std::vector<std::string>& denied) {
  harness::PrefixChecker checker(kGrammar);
  std::string problems;
  if (!checker.TryAppend(prefix)) {
    problems = " prefix refused";
  } else {
    auto mask = harness::ExhaustiveMask(checker, Info().GetDecodedVocab(), Info().GetStopTokenIds());
    for (const auto& t : allowed)
      if (!Allowed(mask, t)) problems += " " + Show(t) + " denied;";
    for (const auto& t : denied)
      if (Allowed(mask, t)) problems += " " + Show(t) + " allowed;";
  }
  printf("%s  %s%s\n", problems.empty() ? "pass" : "FAIL", Show(prefix).c_str(), problems.c_str());
  if (!problems.empty()) ++failures;
}

// `text` up to and including the first `needle`.
static std::string Upto(const std::string& text, const std::string& needle) {
  return text.substr(0, text.find(needle) + needle.size());
}

int main() {
  // The grammar the parser actually uses: the source after XGrammar's optimizations.
  printf("grammar used by the parser (optimized by XGrammar):\n%s\n\n",
         harness::CompileWithoutMasks(kGrammar).GetGrammar().ToString().c_str());

  // The saved Qwen2.5-Coder-1.5B greedy BitWriter::new body: valid syntax, despite its
  // ownership error (`output` is moved by the shorthand field, then used again).
  const std::string body = ReadFile(HARNESS_SOURCE_DIR "/tests/fixtures/bitwriter_new.1.5b.greedy.body.rs");
  Check("", {"{", " {"}, {"```", EOS});
  Check(Upto(body, "cur_bit:"), {" true", " output", " !"}, {" &", " {", EOS});
  Check(Upto(body, "output."), {"is", " "}, {"(", "1"});
  Check(body, {EOS, "\n"}, {"}", ";"});

  // Variables, field labels and shorthand fields are all accepted syntactically.
  Check("{ Self { missing: ", {"flag", "missing"}, {"}"});
  Check("{ Self { missing", {" }", ",", ":"}, {"."});
  Check("{ out", {"put", " }"}, {EOS});  // unfinished name: may continue or end

  // Trivia: \r\n, comments, trailing whitespace.
  Check("\r\n{ // c\r\n\tlet x: usize = 0usize; x }\n", {EOS, "\n"}, {"}"});

  // Keyword boundaries: "letx" is one identifier; return needs no space before ( or !.
  Check("{ letx ", {"}"}, {"="});
  Check("{ return", {"(", "!", " x"}, {";"});
  Check("{ let mut", {" x", " ="}, {});  // " =": keyword as identifier (temporary)

  // Unsupported syntax: block comment, borrow, method argument.
  Check("{ /", {"/"}, {"*"});
  Check("{ Self { len: ", {"!", "("}, {"&"});
  Check("{ x.f(", {")"}, {"1"});

  printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
