// The grammars and hand-written token sequences shared by test_native_equivalence
// and bench_masks.
#pragma once

#include <xgrammar/xgrammar.h>

#include <string>
#include <vector>

namespace harness {

struct GrammarCase {
  std::string name;
  std::string ebnf;
  std::vector<std::string> tokens;  // the hand-written sequence, by decoded bytes
};

inline std::vector<GrammarCase> Cases() {
  return {
      {"let", R"(root ::= "let " [a-z]+ " = " [0-9]+ ";")", {"let", " x", " =", " ", "1", "2", ";"}},
      {"list", R"(root ::= [a-z]+ ("," [a-z]+)*)", {"abc", ",", "de", "f"}},
      {"string",
       R"(root ::= "s = \"" [^"]* "\";")",
       // "é" (C3 A9), "中" (E4 B8 AD), "😀" (F0 9F 98 80), one byte token at a time,
       // then "é" as one token
       {"s", " =", " \"", "\xC3", "\xA9", "\xE4", "\xB8", "\xAD", "\xF0", "\x9F", "\x98", "\x80", "\xC3\xA9",
        "\";"}},
      {"json",
       xgrammar::Grammar::BuiltinJSONGrammar().ToString(),
       {"{\"", "a", "\":", " [", "1", ",", " \"", "\xC3", "\xA9", "\"]", "}"}},
  };
}

}  // namespace harness
