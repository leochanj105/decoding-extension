// The cost of one position, and what it offers there.
//
// bench_e2e.cc measures a whole program, which is the number that matters but hides
// where it comes from. This measures single positions, and exists because a claim
// about one position's cost was wrong twice: that marking an extra rule as
// runtime-supplied "only costs a few tokens being re-checked" (it cost 15 ms), and
// that adding the fresh-name check would push that position back onto the slow path
// (it did not). Both were settled here in one run.
//
// The positions below walk into a declaration: no name yet, one letter, and a
// complete name that is already taken.
#include "fragment_env.h"
#include <chrono>
#include <cstdio>
#include <sstream>
using namespace xgrammar;
using namespace maskgen;
using Clock = std::chrono::steady_clock;
int main(int, char** argv) {
  auto vocab = LoadVocab(argv[1]);
  const int V = (int)vocab.size();
  TokenizerInfo ti(vocab, VocabType::BYTE_LEVEL, V, std::vector<int32_t>{151643});
  std::ifstream gf(std::string(MASKGEN_DIR) + "/fragment.ebnf");
  std::stringstream gs; gs << gf.rdbuf();
  GrammarCompiler c(ti, 8, false);
  auto compiled = c.CompileGrammar(gs.str(), "root");
  TypeTable table; std::string err;
  { std::ifstream tf(std::string(MASKGEN_DIR) + "/fragment.types");
    std::stringstream ts; ts << tf.rdbuf();
    if (!TypeTable::Parse(ts.str(), &table, &err)) { printf("table: %s\n", err.c_str()); return 1; } }
  Environment env; RuleIds rules;
  { GrammarMatcher p(compiled);
    rules.member_name = p.GetRuleId("_lex_member");
    rules.call_step = p.GetRuleId("_call_step");
    rules.member_step = p.GetRuleId("_member_step");
    rules.arr_lit = p.GetRuleId("_arr_lit");
    rules.decl_name = p.GetRuleId("_decl_name"); }
  for (const char* prefix : {"let", "let ", "let c", "let count"}) {
    GrammarMatcher m(compiled);
    Install(m, env, table, rules);
    if (!m.AcceptString(prefix)) { printf("%-12s could not accept\n", prefix); continue; }
    Bitmask bm(V);
    m.FillNextTokenBitmask(&bm.t, 0);
    int allowed = 0, colon = -1;
    const auto& dec = ti.GetDecodedVocab();
    for (int i = 0; i < V; ++i) { if (bm.allows(i)) ++allowed; if (dec[i] == " :") colon = i; }
    const int reps = 20;
    auto t0 = Clock::now();
    for (int r = 0; r < reps; ++r) m.FillNextTokenBitmask(&bm.t, 0);
    auto t1 = Clock::now();
    printf("after \"%-10s\"  %9.1f us  %7d allowed   ' :' %s\n", prefix,
           std::chrono::duration<double, std::micro>(t1 - t0).count() / reps, allowed,
           colon >= 0 ? (bm.allows(colon) ? "allowed" : "refused") : "n/a");
  }
  return 0;
}
