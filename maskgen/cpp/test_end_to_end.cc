// A whole program, one token at a time, through the real grammar file.
//
// Every other test here uses a small grammar written inline to isolate one
// mechanism. This one uses maskgen/fragment.ebnf as it ships, and drives the
// matcher the way a decoder would: the text is cut into vocabulary tokens by
// longest match, and at every step the token the program needs must be in the
// mask before it is accepted. Tokens that straddle a grammar boundary -- ` msg`
// carries the space, the name, and nothing else -- are therefore exercised
// throughout rather than being set up deliberately.
//
// The environment is fixed. Nothing yet feeds a `let` declaration back into the
// symbol table, so the programs below use the three names given here and never
// refer to one they declare.
//
//   count, total : number        msg : string
//
// No type in this environment is callable, so a call step is always refused and
// a member is written without one: `count.toString` and not `count.toString()`.
#include <xgrammar/xgrammar.h>
#include <dlpack/dlpack.h>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace xgrammar;

static std::vector<std::string> LoadVocab(const std::string& p) {
  std::ifstream f(p); std::string line; std::getline(f, line);
  int n = std::stoi(line); std::vector<std::string> v; v.reserve(n);
  for (int i = 0; i < n; ++i) { std::getline(f, line); v.push_back(line); }
  return v;
}

static int failures = 0;
static void check(bool ok, const std::string& what) {
  printf("%s  %s\n", ok ? "pass" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

struct Bitmask {
  std::vector<int32_t> buf; DLTensor t{}; int64_t shape[1];
  explicit Bitmask(int vocab) {
    buf.assign(GetBitmaskSize(vocab), 0);
    shape[0] = static_cast<int64_t>(buf.size());
    t.data = buf.data(); t.device = DLDevice{kDLCPU, 0}; t.ndim = 1;
    t.dtype = GetBitmaskDLType(); t.shape = shape; t.strides = nullptr; t.byte_offset = 0;
  }
  bool allows(int id) const { return buf[id / 32] & (1 << (id % 32)); }
};

enum { T_NUM = 0, T_STR = 1, T_BOOL = 2, T_NUMA = 3, T_STRA = 4, T_BOOLA = 5, T_NTYPES = 6 };
// The requirement at a position that imposes none, such as an operand of ==.
enum { T_ANY = T_NTYPES, T_NGROUPS = T_NTYPES + 1 };
static const char* kTypeNames[] = {
    "number", "string", "boolean", "number[]", "string[]", "boolean[]"};
static int32_t MemberTag(int32_t recv, int32_t need) { return 100 + recv * T_NGROUPS + need; }

namespace {

/*! \brief The type environment: names, members, and which type reaches which. */
struct Environment {
  std::map<std::string, int32_t> symbols{
      {"count", T_NUM}, {"total", T_NUM}, {"msg", T_STR}};
  std::map<int32_t, std::map<std::string, int32_t>> members{
      {T_NUM, {{"toString", T_STR}, {"toFixed", T_STR}, {"valueOf", T_NUM}}},
      {T_STR, {{"length", T_NUM}, {"toUpperCase", T_STR}, {"split", T_STRA}}},
      {T_BOOL, {{"toString", T_STR}}},
      {T_NUMA, {{"length", T_NUM}, {"join", T_STR}}},
      {T_STRA, {{"length", T_NUM}, {"join", T_STR}}},
      {T_BOOLA, {{"length", T_NUM}, {"join", T_STR}}}};
  // Which source types can reach each target, i.e. the transitive closure of the
  // member edges above. A name is admitted at a position if its type reaches what
  // the position requires, since a member access may still get there.
  std::map<int32_t, std::vector<int32_t>> reaches{
      {T_NUM, {T_NUM, T_STR, T_NUMA, T_STRA, T_BOOLA}},
      {T_STR, {T_NUM, T_STR, T_BOOL, T_NUMA, T_STRA, T_BOOLA}},
      {T_BOOL, {T_BOOL}},
      {T_NUMA, {T_NUMA}},
      {T_STRA, {T_STR, T_STRA}},
      {T_BOOLA, {T_BOOLA}}};

  bool reachable(int32_t from, int32_t to) const {
    auto it = reaches.find(to);
    if (it == reaches.end()) return false;
    for (int32_t s : it->second) if (s == from) return true;
    return false;
  }
};

/*! \brief Install an environment on a matcher, including the fixed-type constructs. */
void Install(GrammarMatcher& m, const Environment& env, int32_t member_rule,
             int32_t call_rule, const std::map<int32_t, int32_t>& produced) {
  for (int32_t t = 0; t < T_NTYPES; ++t) {
    std::vector<std::string> names;
    for (const auto& [n, ty] : env.symbols) if (ty == t) names.push_back(n);
    m.SetLexiconNames(t, names);
    m.SetLexiconReachableTags(t, env.reaches.at(t));
  }
  // The any group holds no names of its own; every type's names can reach it.
  m.SetLexiconNames(T_ANY, {});
  m.SetLexiconReachableTags(T_ANY, {T_NUM, T_STR, T_BOOL, T_NUMA, T_STRA, T_BOOLA});
  for (int32_t r = 0; r < T_NTYPES; ++r) {
    for (int32_t need = 0; need < T_NGROUPS; ++need) {
      std::vector<std::string> ok;
      for (const auto& [name, mt] : env.members.at(r)) {
        if (need == T_ANY || env.reachable(mt, need)) ok.push_back(name);
      }
      m.SetLexiconNames(MemberTag(r, need), ok);
      m.SetLexiconReachableTags(MemberTag(r, need), {MemberTag(r, need)});
    }
  }
  m.SetTypeResolver([&env, produced](int32_t rule, std::string_view text) -> int32_t {
    // A construct with a fixed type answers whatever its text, including no text:
    // that is how prediction learns the type without waiting for the match.
    auto fixed = produced.find(rule);
    if (fixed != produced.end()) return fixed->second;
    for (int32_t t = 0; t < T_NTYPES; ++t) if (text == kTypeNames[t]) return t;
    auto sym = env.symbols.find(std::string(text));
    if (sym != env.symbols.end()) return sym->second;
    for (const auto& [recv, ms] : env.members) {
      auto mem = ms.find(std::string(text));
      if (mem != ms.end()) return mem->second;
    }
    return -1;
  });
  m.SetTypeAcceptor([](int32_t need, int32_t have) { return need == have; });
  m.SetLexiconTagResolver([member_rule](int32_t rule, int32_t need, int32_t have) -> int32_t {
    if (rule == member_rule) {
      if (have < 0) return -1;                       // no receiver, so no members
      return MemberTag(have, need < 0 ? T_ANY : need);
    }
    return need < 0 ? T_ANY : need;
  });
  m.SetStepPredicate([call_rule](int32_t rule, int32_t, int32_t have) {
    if (rule == call_rule) return false;             // nothing here is callable
    if (rule == -1) return true;
    return have >= 0;                                // a member needs a receiver
  });
}

/*! \brief Cut text into vocabulary tokens by longest match, as a decoder's output would already be. */
std::vector<int32_t> Tokenize(
    const std::string& text, const std::unordered_map<std::string, int32_t>& by_text
) {
  std::vector<int32_t> out;
  size_t i = 0;
  while (i < text.size()) {
    size_t len = std::min<size_t>(24, text.size() - i);
    for (; len > 0; --len) {
      auto it = by_text.find(text.substr(i, len));
      if (it != by_text.end()) { out.push_back(it->second); break; }
    }
    if (len == 0) { out.clear(); return out; }        // not tokenizable
    i += len;
  }
  return out;
}

}  // namespace

int main(int, char** argv) {
  auto vocab = LoadVocab(argv[1]);
  const int V = static_cast<int>(vocab.size());
  TokenizerInfo ti(vocab, VocabType::BYTE_LEVEL, V, std::vector<int32_t>{151643});
  const auto& decoded = ti.GetDecodedVocab();
  std::unordered_map<std::string, int32_t> by_text;
  for (int32_t i = 0; i < V; ++i) by_text.emplace(decoded[i], i);

  std::ifstream gf(std::string(MASKGEN_DIR) + "/fragment.ebnf");
  std::stringstream gs; gs << gf.rdbuf();
  check(!gs.str().empty(), "the real grammar file loads");
  GrammarCompiler compiler(ti, 8, false);
  auto compiled = compiler.CompileGrammar(gs.str(), "root");

  Environment env;
  int32_t member_rule, call_rule;
  std::map<int32_t, int32_t> produced;
  {
    GrammarMatcher probe(compiled);
    member_rule = probe.GetRuleId("__lex_member");
    call_rule = probe.GetRuleId("__step_call");
    for (const auto& [name, ty] : std::map<std::string, int32_t>{
             {"__have_num_lit", T_NUM}, {"__have_str_lit", T_STR},
             {"__have_bool_lit", T_BOOL}, {"__have_cmp", T_BOOL}}) {
      int32_t id = probe.GetRuleId(name);
      check(id >= 0, "the grammar has a rule named " + name);
      if (id >= 0) produced[id] = ty;
    }
    check(member_rule >= 0 && call_rule >= 0, "the grammar has __lex_member and __step_call");
  }

  // A well-typed program. Each statement needs a different part of the machinery:
  // a name of the required type, a member access that converts one type to
  // another, a comparison, a literal, and a sum.
  const std::string program =
      "let a : string = msg ;\n"
      "let b : number = msg.length ;\n"
      "let c : boolean = count == total ;\n"
      "let d : string = count.toString ;\n"
      "let e : number = 3 + count ;\n"
      "let f : string[] = msg.split ;\n";

  auto tokens = Tokenize(program, by_text);
  check(!tokens.empty(), "the program cuts into vocabulary tokens");

  {
    GrammarMatcher m(compiled);
    Install(m, env, member_rule, call_rule, produced);
    Bitmask mask(V);
    size_t accepted = 0;
    std::string refused_at;
    for (int32_t id : tokens) {
      m.FillNextTokenBitmask(&mask.t, 0);
      if (!mask.allows(id)) {
        refused_at = program.substr(0, 0);
        for (size_t k = 0; k < accepted; ++k) refused_at += decoded[tokens[k]];
        refused_at += " <<[" + decoded[id] + "]";
        break;
      }
      if (!m.AcceptToken(id)) { refused_at = "AcceptToken disagreed with the mask"; break; }
      ++accepted;
    }
    check(accepted == tokens.size(),
          "every token of the program is in the mask (" + std::to_string(accepted) + "/" +
              std::to_string(tokens.size()) + ")" +
              (refused_at.empty() ? "" : "  stopped: " + refused_at));
    check(m.IsTerminated() || accepted == tokens.size(), "the program is complete");
  }

  // Each of these is grammatical and every one is a type error. The mask must stop
  // the program somewhere, and the position is named so a change of behaviour shows
  // up as a different position rather than a silent pass.
  const std::vector<std::pair<std::string, std::string>> ill_typed = {
      {"let a : string = count ;\n", "count is a number, so the ';' must be refused"},
      {"let a : number = msg ;\n", "msg is a string, so the ';' must be refused"},
      {"let a : number[] = msg ;\n", "nothing reaches number[] from a string"},
      {"let a : string = 3 ;\n", "a numeric literal is not a string"},
      {"let a : number = true ;\n", "a boolean literal is not a number"},
      {"let a : number = count == total ;\n", "a comparison is a boolean, not a number"},
      {"let a : number = msg.toUpperCase ;\n", "toUpperCase gives a string, not a number"},
      {"let a : string = count.valueOf ;\n", "valueOf gives a number, not a string"},
      {"let a : number = count(total) ;\n", "count is not callable"},
      {"let a : number = count.nosuch ;\n", "number has no member called nosuch"},
  };
  for (const auto& [text, why] : ill_typed) {
    auto ill = Tokenize(text, by_text);
    GrammarMatcher m(compiled);
    Install(m, env, member_rule, call_rule, produced);
    Bitmask mask(V);
    bool blocked = false;
    for (int32_t id : ill) {
      m.FillNextTokenBitmask(&mask.t, 0);
      if (!mask.allows(id) || !m.AcceptToken(id)) { blocked = true; break; }
    }
    check(blocked, why);
  }

  // The same programs with the requirement satisfied, to show the refusals above
  // come from the types and not from a syntax the grammar simply cannot express.
  const std::vector<std::string> well_typed = {
      "let a : number = count ;\n",
      "let a : string = msg ;\n",
      "let a : string[] = msg.split ;\n",
      "let a : number = 3 ;\n",
      "let a : boolean = true ;\n",
      "let a : boolean = count == total ;\n",
      "let a : string = msg.toUpperCase ;\n",
      "let a : number = count.valueOf ;\n",
  };
  for (const auto& text : well_typed) {
    auto ok = Tokenize(text, by_text);
    GrammarMatcher m(compiled);
    Install(m, env, member_rule, call_rule, produced);
    Bitmask mask(V);
    bool all = true;
    std::string got;
    for (int32_t id : ok) {
      m.FillNextTokenBitmask(&mask.t, 0);
      if (!mask.allows(id) || !m.AcceptToken(id)) {
        all = false;
        got += " <<[" + decoded[id] + "]";
        break;
      }
      got += decoded[id];
    }
    check(all, "accepted: " + text.substr(0, text.size() - 1) +
                   (all ? "" : "  stopped: " + got));
  }

  printf("\n%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
