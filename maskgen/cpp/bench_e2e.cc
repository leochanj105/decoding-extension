// End-to-end mask-generation latency, measured the way a decoder pays it.
//
// Every earlier number here came from one position measured in a loop, which
// answers "how fast is this position when nothing else is going on". A decoder
// never does that: it pays a mask at every position of a program, once each, in
// order, with the parser in whatever state the previous token left it. The slow
// positions are the ones that matter, because one slow step stalls a pipeline, and
// a mean over a loop at a fast position hides them.
//
// So: take a program, cut it into real vocabulary tokens, and at every step measure
// the mask, the accept, and how many tokens came back. Repeat the whole program with
// a fresh matcher to average out noise -- never by re-measuring one position, which
// is the thing being avoided. Then do it again with no type table installed, which
// is plain XGrammar on the same grammar, and the difference is what the types cost.
#include "fragment_env.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace xgrammar;
using namespace maskgen;
using Clock = std::chrono::steady_clock;

namespace {

/*! \brief What one position of the program cost. */
struct Step {
  std::string written;   // the token the program needed here
  double mask_us = 0;    // time to produce the mask
  double accept_us = 0;  // time to advance past the token
  int allowed = 0;       // tokens the mask permitted
};

double Micros(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::micro>(b - a).count();
}

/*! \brief A percentile of a sorted copy. */
double Pct(std::vector<double> xs, double p) {
  if (xs.empty()) return 0;
  std::sort(xs.begin(), xs.end());
  const size_t i = static_cast<size_t>(p * (xs.size() - 1) + 0.5);
  return xs[i];
}

/*!
 * \brief Walk the program once, timing every position.
 *
 * `install` is what distinguishes the two runs: the full environment, or nothing at
 * all, which leaves plain XGrammar on the same grammar.
 */
std::vector<Step> WalkOnce(
    const CompiledGrammar& compiled, const std::vector<int32_t>& tokens,
    const std::vector<std::string>& decoded, int vocab_size,
    const std::function<void(GrammarMatcher&)>& install
) {
  GrammarMatcher m(compiled);
  install(m);
  Bitmask bm(vocab_size);
  std::vector<Step> steps;
  steps.reserve(tokens.size());
  for (int32_t id : tokens) {
    Step step;
    step.written = decoded[id];

    auto t0 = Clock::now();
    m.FillNextTokenBitmask(&bm.t, 0);
    auto t1 = Clock::now();
    step.mask_us = Micros(t0, t1);

    for (int i = 0; i < vocab_size; ++i) {
      if (bm.allows(i)) ++step.allowed;
    }
    if (!bm.allows(id)) {
      printf("  !! the program's own token [%s] was refused at step %zu\n",
             step.written.c_str(), steps.size());
      return steps;
    }

    t0 = Clock::now();
    const bool ok = m.AcceptToken(id);
    t1 = Clock::now();
    step.accept_us = Micros(t0, t1);
    if (!ok) {
      printf("  !! AcceptToken disagreed with the mask at step %zu\n", steps.size());
      return steps;
    }
    steps.push_back(step);
  }
  return steps;
}

/*! \brief Average each position over `runs` fresh walks. */
std::vector<Step> Walk(
    const CompiledGrammar& compiled, const std::vector<int32_t>& tokens,
    const std::vector<std::string>& decoded, int vocab_size,
    const std::function<void(GrammarMatcher&)>& install, int runs
) {
  std::vector<Step> total = WalkOnce(compiled, tokens, decoded, vocab_size, install);
  if (total.size() != tokens.size()) return total;
  for (int r = 1; r < runs; ++r) {
    auto again = WalkOnce(compiled, tokens, decoded, vocab_size, install);
    for (size_t i = 0; i < total.size(); ++i) {
      total[i].mask_us += again[i].mask_us;
      total[i].accept_us += again[i].accept_us;
    }
  }
  for (auto& s : total) {
    s.mask_us /= runs;
    s.accept_us /= runs;
  }
  return total;
}

void Report(const char* label, const std::vector<Step>& steps) {
  std::vector<double> mask, accept;
  double mask_total = 0, accept_total = 0;
  for (const auto& s : steps) {
    mask.push_back(s.mask_us);
    accept.push_back(s.accept_us);
    mask_total += s.mask_us;
    accept_total += s.accept_us;
  }
  printf("\n%s  (%zu tokens)\n", label, steps.size());
  printf("  mask     mean %7.1f us   median %7.1f   p90 %7.1f   max %7.1f   total %8.1f\n",
         mask_total / steps.size(), Pct(mask, 0.5), Pct(mask, 0.9), Pct(mask, 1.0), mask_total);
  printf("  accept   mean %7.1f us   median %7.1f   p90 %7.1f   max %7.1f   total %8.1f\n",
         accept_total / steps.size(), Pct(accept, 0.5), Pct(accept, 0.9), Pct(accept, 1.0),
         accept_total);
  // A model step is about 33 ms. What matters is the fraction of one step, since the
  // mask is on the critical path between one token and the next.
  printf("  worst position is %.2f%% of a 33 ms model step; the mean is %.3f%%\n",
         Pct(mask, 1.0) / 330.0, (mask_total / steps.size()) / 330.0);
}

std::string Escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\n') out += "\\n";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  auto vocab = LoadVocab(argv[1]);
  const int V = static_cast<int>(vocab.size());
  TokenizerInfo ti(vocab, VocabType::BYTE_LEVEL, V, std::vector<int32_t>{151643});
  const auto& decoded = ti.GetDecodedVocab();

  std::ifstream gf(std::string(MASKGEN_DIR) + "/fragment.ebnf");
  std::stringstream gs; gs << gf.rdbuf();
  auto t0 = Clock::now();
  GrammarCompiler compiler(ti, 8, false);
  auto compiled = compiler.CompileGrammar(gs.str(), "root");
  auto t1 = Clock::now();
  printf("compiling the grammar: %.1f ms (once, before anything generates)\n",
         Micros(t0, t1) / 1000.0);

  TypeTable table;
  {
    std::ifstream tf(std::string(MASKGEN_DIR) + "/fragment.types");
    std::stringstream ts; ts << tf.rdbuf();
    std::string error;
    if (!TypeTable::Parse(ts.str(), &table, &error)) {
      printf("the type table did not parse: %s\n", error.c_str());
      return 1;
    }
  }

  Environment env;
  RuleIds rules;
  {
    GrammarMatcher probe(compiled);
    rules.member_name = probe.GetRuleId("_member_name");
    rules.call_step = probe.GetRuleId("_call_step");
    rules.member_step = probe.GetRuleId("_member_step");
  }

  // A program that uses every construct the fragment has, so the distribution is not
  // dominated by one kind of position.
  const std::string program =
      "let total : number = count + 3 ;\n"
      "let label : string = msg + count ;\n"
      "let parts : string[] = msg.split ;\n"
      "let size : number = msg.split.length ;\n"
      "msg = count.toString ;\n"
      "let same : boolean = count == total ;\n"
      "let text : string = ( msg ) + msg.toUpperCase ;\n";

  std::unordered_map<std::string, int32_t> by_text;
  for (int32_t i = 0; i < V; ++i) by_text.emplace(decoded[i], i);
  auto tokens = Tokenize(program, by_text);
  if (tokens.empty()) {
    printf("the program does not cut into vocabulary tokens\n");
    return 1;
  }

  const int runs = argc > 2 ? std::atoi(argv[2]) : 20;
  printf("program: %zu tokens, averaged over %d walks with a fresh matcher each time\n",
         tokens.size(), runs);

  const auto with_types = [&](GrammarMatcher& m) { Install(m, env, table, rules); };
  const auto without = [](GrammarMatcher&) {};

  // A third configuration, and the only fair one for an overall number. "No table on
  // this grammar" is not plain XGrammar: the externally-named rules are still
  // downgraded at compile time so that every candidate is walked, and with no veto to
  // prune them that is the worst of both worlds. Stripping the underscores gives the
  // same language with none of this machinery in it at all.
  std::string stripped = gs.str();
  {
    std::vector<std::string> names = table.ListedRules();
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
      return a.size() > b.size();  // longest first, or _name rewrites _member_name
    });
    for (const std::string& name : names) {
      const std::string bare = name.substr(1);
      for (size_t at = stripped.find(name); at != std::string::npos;
           at = stripped.find(name, at + bare.size())) {
        stripped.replace(at, name.size(), bare);
      }
    }
  }
  auto bare_compiled = compiler.CompileGrammar(stripped, "root");

  auto typed = Walk(compiled, tokens, decoded, V, with_types, runs);
  auto plain = Walk(compiled, tokens, decoded, V, without, runs);
  auto bare = Walk(bare_compiled, tokens, decoded, V, without, runs);
  if (typed.size() != tokens.size() || plain.size() != tokens.size()) {
    printf("the walk did not finish; nothing to report\n");
    return 1;
  }

  if (bare.size() != tokens.size()) {
    printf("the stripped grammar did not accept the program; skipping that baseline\n");
    bare = plain;
  }
  Report("with the type table", typed);
  Report("same grammar, no type table installed", plain);
  Report("underscores stripped: the same language, none of this machinery", bare);

  double typed_total = 0, plain_total = 0, bare_total = 0;
  for (size_t i = 0; i < typed.size(); ++i) {
    typed_total += typed[i].mask_us;
    plain_total += plain[i].mask_us;
    bare_total += bare[i].mask_us;
  }
  printf("\nover the whole program: %.0f us with types, %.0f us with none of this,"
         " %.2fx\n", typed_total, bare_total, typed_total / bare_total);
  printf("the middle configuration costs %.0f us, which is what downgrading a position"
         " to \"check every candidate\" costs when nothing prunes it\n", plain_total);

  // The positions that would stall a pipeline, and what the model sees there.
  std::vector<size_t> order(typed.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return typed[a].mask_us > typed[b].mask_us;
  });
  printf("\nthe ten slowest positions\n");
  printf("  %-30s %10s %10s %8s %9s\n", "written so far", "with types", "bare", "ratio",
         "allowed");
  for (int i = 0; i < 10 && i < static_cast<int>(order.size()); ++i) {
    const size_t at = order[i];
    std::string prefix;
    for (size_t k = 0; k < at; ++k) prefix += typed[k].written;
    if (prefix.size() > 26) prefix = "..." + prefix.substr(prefix.size() - 23);
    printf("  %-30s %8.1fus %8.1fus %7.2fx %9d\n", Escape(prefix).c_str(), typed[at].mask_us,
           bare[at].mask_us, typed[at].mask_us / std::max(bare[at].mask_us, 0.01),
           typed[at].allowed);
  }
  return 0;
}
