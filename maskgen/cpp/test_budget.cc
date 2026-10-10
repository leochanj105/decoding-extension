// A ceiling on what a mask may cost.
//
// Every other test here checks answers. This one checks the clock, because a mask
// that is a thousand times too slow gives exactly the right answer and passes
// everything else. That happened: a rule wrongly marked as drawing on the symbol
// table took one position from 9 us to 15 ms, correct throughout, and surfaced only
// because someone asked for a profile.
//
// The bounds are deliberately loose -- roughly ten times the measured cost -- so
// that a loaded machine cannot fail them while a regression of the kind above still
// does. bench_e2e.cc is where the real numbers live; this is only a tripwire.
#include "fragment_env.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <unordered_map>
#include <vector>

using namespace xgrammar;
using namespace maskgen;
using Clock = std::chrono::steady_clock;

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
  auto t0 = Clock::now();
  GrammarCompiler compiler(ti, 8, false);
  auto compiled = compiler.CompileGrammar(gs.str(), "root");
  const double compile_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

  TypeTable table;
  {
    std::ifstream tf(std::string(MASKGEN_DIR) + "/fragment.types");
    std::stringstream ts; ts << tf.rdbuf();
    std::string error;
    if (!TypeTable::Parse(ts.str(), &table, &error)) {
      printf("FAIL  fragment.types parses: %s\n", error.c_str());
      return 1;
    }
  }

  Environment start;
  RuleIds rules;
  {
    GrammarMatcher probe(compiled);
    rules.member_name = probe.GetRuleId("_lex_member");
    rules.call_step = probe.GetRuleId("_call_step");
    rules.member_step = probe.GetRuleId("_member_step");
    rules.arr_lit = probe.GetRuleId("_arr_lit");
    rules.decl_name = probe.GetRuleId("_decl_name");
  }

  // Every construct the fragment has, including the positions that have been
  // expensive before: a brand-new name after `let`, and a name being used.
  const std::string program =
      "let sum : number = count + 3 ;\n"
      "let label : string = msg + count ;\n"
      "let parts : string[] = msg.split(msg) ;\n"
      "let size : number = msg.split(msg).length ;\n"
      "msg = count.toString() ;\n"
      "let same : boolean = count == total ;\n"
      "let text : string = ( msg ) + msg.toUpperCase() ;\n"
      "let nums : number[] = [ 1 , 2 , count ] ;\n"
      "let empty : string[] = [] ;\n"
      "let used : string = label + sum ;\n";

  const auto tokens = Tokenize(program, by_text);
  if (tokens.empty()) {
    printf("FAIL  the program cuts into vocabulary tokens\n");
    return 1;
  }

  // Five walks with a fresh matcher, taking the BEST time per position: the cheapest
  // observation is the one least polluted by whatever else the machine is doing.
  std::vector<double> best(tokens.size(), 1e9);
  for (int run = 0; run < 5; ++run) {
    GrammarMatcher m(compiled);
    Environment env = start;
    Install(m, env, table, rules);
    Declarations declared(&env, &m);
    Bitmask bm(V);
    for (size_t i = 0; i < tokens.size(); ++i) {
      auto a = Clock::now();
      m.FillNextTokenBitmask(&bm.t, 0);
      auto b = Clock::now();
      best[i] = std::min(best[i], std::chrono::duration<double, std::micro>(b - a).count());
      if (!bm.allows(tokens[i]) || !m.AcceptToken(tokens[i])) {
        printf("FAIL  the program's own token [%s] was refused at step %zu\n",
               decoded[tokens[i]].c_str(), i);
        return 1;
      }
      declared.Poll();
    }
  }

  double total = 0, worst = 0;
  size_t worst_at = 0;
  for (size_t i = 0; i < best.size(); ++i) {
    total += best[i];
    if (best[i] > worst) {
      worst = best[i];
      worst_at = i;
    }
  }
  const double mean = total / best.size();

  std::string worst_prefix;
  for (size_t k = 0; k < worst_at; ++k) worst_prefix += decoded[tokens[k]];
  if (worst_prefix.size() > 30) worst_prefix = "..." + worst_prefix.substr(worst_prefix.size() - 27);
  for (char& c : worst_prefix) {
    if (c == '\n') c = ' ';
  }

  printf("     %zu tokens: mean %.1f us, worst %.1f us after \"%s\", compile %.0f ms\n",
         tokens.size(), mean, worst, worst_prefix.c_str(), compile_ms);

  // Measured at the time of writing: mean 160 us, worst 979 us, compile 120 ms.
  check(mean < 2000.0, "the mean mask is under 2 ms, measured " + std::to_string((int)mean) + " us");
  check(worst < 8000.0,
        "no single mask exceeds 8 ms, worst was " + std::to_string((int)worst) + " us");
  check(compile_ms < 1500.0,
        "compiling stays under 1.5 s, took " + std::to_string((int)compile_ms) + " ms");

  printf("\n%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
