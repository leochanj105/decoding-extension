// The two ways of feeding the matcher must agree, at every position.
//
// XGrammar accepts input either one byte at a time or one token at a time, and a
// decoder only ever does the second. Four bugs this project hit came from a shortcut
// behaving differently for tokens than for bytes -- one parser position per token
// instead of per byte, an in-flight byte buffer filled only for an unrelated
// feature, a rule's completion skipped, a rule folded away. Each was found by hand.
//
// This is the net under the whole category: walk a program both ways and require the
// masks to be identical at every step. Any future shortcut that behaves differently
// for tokens fails here the day it lands.
#include "fragment_env.h"

#include <cstdio>
#include <sstream>
#include <unordered_map>
#include <vector>

using namespace xgrammar;
using namespace maskgen;

static int failures = 0;
static void check(bool ok, const std::string& what) {
  printf("%s  %s\n", ok ? "pass" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

int main(int, char** argv) {
  auto vocab = LoadVocab(argv[1]);
  const int V = static_cast<int>(vocab.size());
  TokenizerInfo ti(vocab, VocabType::BYTE_LEVEL, V, std::vector<int32_t>{151643});
  const auto& decoded = ti.GetDecodedVocab();
  std::unordered_map<std::string, int32_t> by_text;
  for (int32_t i = 0; i < V; ++i) by_text.emplace(decoded[i], i);

  std::ifstream gf(std::string(MASKGEN_DIR) + "/fragment.ebnf");
  std::stringstream gs; gs << gf.rdbuf();
  GrammarCompiler compiler(ti, 8, false);
  auto compiled = compiler.CompileGrammar(gs.str(), "root");

  TypeTable table;
  {
    std::ifstream tf(std::string(MASKGEN_DIR) + "/fragment.types");
    std::stringstream ts; ts << tf.rdbuf();
    std::string error;
    check(TypeTable::Parse(ts.str(), &table, &error), "fragment.types parses: " + error);
  }

  Environment start;
  RuleIds rules;
  {
    GrammarMatcher probe(compiled);
    rules.member_name = probe.GetRuleId("_lex_member");
    rules.call_step = probe.GetRuleId("_call_step");
    rules.member_step = probe.GetRuleId("_member_step");
    rules.arr_lit = probe.GetRuleId("_arr_lit");
    rules.arr_empty = probe.GetRuleId("_arr_empty");
    rules.decl_name = probe.GetRuleId("_decl_name");
  }

  // Programs chosen so that tokens straddle boundaries in different ways: a space
  // glued to a name, a dot glued to a member, parentheses, brackets, and a run with
  // no whitespace at all.
  const std::vector<std::string> programs = {
      "let a : string = msg ;\n",
      "let b : number = msg.length ;\n",
      "let c : string = msg.split(msg).join(msg) ;\n",
      "let d : number[] = [ 1 , count , 3 ] ;\n",
      "let e : string = ( msg ) + count.toString() ;\n",
      "let f:number=count;msg=count.toString();",
      "declare function g ( a : number ) : boolean ;\nlet h : boolean = g(count) ;\n",
      "let n : number = 3 ;\nlet s : string = n.toString() ;\nlet t : string = s + n ;\n",
  };

  for (const std::string& program : programs) {
    const auto tokens = Tokenize(program, by_text);
    if (tokens.empty()) {
      check(false, "program cuts into tokens: " + program);
      continue;
    }

    // One matcher fed whole tokens, one fed the same bytes one at a time. Both get
    // the full environment and both register declarations, so any difference is the
    // input path and nothing else.
    GrammarMatcher by_token(compiled);
    Environment token_env = start;
    Install(by_token, token_env, table, rules);
    Declarations token_decls(&token_env, &by_token);

    GrammarMatcher by_byte(compiled);
    Environment byte_env = start;
    Install(by_byte, byte_env, table, rules);
    Declarations byte_decls(&byte_env, &by_byte);

    Bitmask token_mask(V), byte_mask(V);
    bool agreed = true;
    std::string written;
    for (int32_t id : tokens) {
      by_token.FillNextTokenBitmask(&token_mask.t, 0);
      by_byte.FillNextTokenBitmask(&byte_mask.t, 0);
      if (token_mask.buf != byte_mask.buf) {
        int only_token = 0, only_byte = 0;
        std::string examples;
        for (int i = 0; i < V; ++i) {
          const bool t = token_mask.allows(i), b = byte_mask.allows(i);
          if (t && !b) {
            ++only_token;
            if (examples.size() < 40) examples += " token-only[" + decoded[i] + "]";
          } else if (b && !t) {
            ++only_byte;
            if (examples.size() < 40) examples += " byte-only[" + decoded[i] + "]";
          }
        }
        check(false, "masks differ after \"" + written + "\": " +
                         std::to_string(only_token) + " token-only, " +
                         std::to_string(only_byte) + " byte-only;" + examples);
        agreed = false;
        break;
      }
      if (!by_token.AcceptToken(id)) {
        check(false, "the token path rejected its own program after \"" + written + "\"");
        agreed = false;
        break;
      }
      if (!by_byte.AcceptString(decoded[id])) {
        check(false, "the byte path rejected its own program after \"" + written + "\"");
        agreed = false;
        break;
      }
      token_decls.Poll();
      byte_decls.Poll();
      written += decoded[id];
    }
    if (agreed) {
      std::string label;
      for (char c : program) label += (c == '\n') ? ' ' : c;
      if (label.size() > 52) label = label.substr(0, 49) + "...";
      check(true, "identical masks at all " + std::to_string(tokens.size()) +
                      " positions: " + label);
    }
  }

  printf("\n%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
