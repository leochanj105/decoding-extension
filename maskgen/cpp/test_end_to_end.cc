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

#include "type_table.h"
#include "type_table_bind.h"
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace xgrammar;
using namespace maskgen;

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

  /*! \brief The type of `name` as a member of `receiver`, or -1. */
  int32_t member_type(int32_t receiver, std::string_view name) const {
    auto group = members.find(receiver);
    if (group == members.end()) return -1;
    auto it = group->second.find(std::string(name));
    return it == group->second.end() ? -1 : it->second;
  }

  bool reachable(int32_t from, int32_t to) const {
    auto it = reaches.find(to);
    if (it == reaches.end()) return false;
    for (int32_t s : it->second) if (s == from) return true;
    return false;
  }
};

/*! \brief The rules this environment has to recognise by name. */
struct RuleIds {
  int32_t member_name = -1;
  int32_t call_step = -1;
  int32_t member_step = -1;
};

/*! \brief What the type table needs from this environment. */
inline TypeOracle OracleFor(const Environment& env) {
  TypeOracle oracle;
  oracle.resolve = [&env](std::string_view text) -> int32_t {
    for (int32_t t = 0; t < T_NTYPES; ++t) {
      if (text == kTypeNames[t]) return t;
    }
    auto sym = env.symbols.find(std::string(text));
    return sym == env.symbols.end() ? -1 : sym->second;
  };
  oracle.accepts = [](int32_t required, int32_t produced) { return required == produced; };
  return oracle;
}

/*! \brief Install an environment on a matcher, driven by the type table. */
void Install(
    GrammarMatcher& m, const Environment& env, const TypeTable& table, const RuleIds& rules
) {
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

  const TypeOracle oracle = OracleFor(env);
  std::vector<RuleTypeTransition> transitions;
  std::string error;
  if (!BuildTransitions(table, m, oracle, &transitions, &error)) {
    printf("FAIL  the table does not match the grammar: %s\n", error.c_str());
    return;
  }
  m.SetRuleTypeTransitions(std::move(transitions));

  // What a piece of matched text means. A member name resolves against the type it
  // is a member of, which is whatever the expression has produced so far.
  m.SetTypeResolver(
      [&env, rules](int32_t rule, std::string_view text, int32_t, int32_t have) -> int32_t {
        if (rule == rules.member_name) return env.member_type(have, text);
        for (int32_t t = 0; t < T_NTYPES; ++t) {
          if (text == kTypeNames[t]) return t;
        }
        auto sym = env.symbols.find(std::string(text));
        return sym == env.symbols.end() ? -1 : sym->second;
      }
  );
  m.SetTypeAcceptor([](int32_t required, int32_t produced) { return required == produced; });
  // Which group of names a position draws from. A member position draws from the
  // members of the receiver that can still reach what the position requires.
  m.SetLexiconTagResolver(
      [rules](int32_t rule, int32_t need, int32_t have) -> int32_t {
        if (rule == rules.member_name) {
          if (have < 0) return -1;              // no receiver, so no members
          return MemberTag(have, need < 0 ? T_ANY : need);
        }
        return need < 0 ? T_ANY : need;
      }
  );
  // Nothing in this environment is callable, and a member needs a receiver.
  m.SetStepPredicate([rules](int32_t rule, int32_t, int32_t have) {
    if (rule == rules.call_step) return false;
    if (rule == rules.member_step) return have >= 0;
    return true;
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

  TypeTable table;
  {
    std::ifstream tf(std::string(MASKGEN_DIR) + "/fragment.types");
    std::stringstream ts; ts << tf.rdbuf();
    std::string error;
    check(TypeTable::Parse(ts.str(), &table, &error), "fragment.types parses: " + error);
  }

  Environment env;
  RuleIds rules;
  {
    GrammarMatcher probe(compiled);
    rules.member_name = probe.GetRuleId("_member_name");
    rules.call_step = probe.GetRuleId("_call_step");
    rules.member_step = probe.GetRuleId("_member_step");
    check(rules.member_name >= 0 && rules.call_step >= 0 && rules.member_step >= 0,
          "the grammar defines the rules this environment recognises");
    std::vector<RuleTypeTransition> transitions;
    std::string error;
    check(BuildTransitions(table, probe, OracleFor(env), &transitions, &error),
          "every row of the table names a rule of the grammar: " + error);
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
    Install(m, env, table, rules);
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
      {"msg = count ;\n", "msg is a string, so assigning a number must be refused"},
      {"count = msg ;\n", "count is a number, so assigning a string must be refused"},
      {"let a : number = ( msg ) ;\n", "a parenthesised string is still not a number"},
      {"let a : string = ( count ) ;\n", "nor the other way round"},
      {"let a : string = msg.split.length ;\n", "a chain ends at number, not string"},
      {"let a:string=count;", "the same holds with no whitespace at all"},
      // No name in this environment is a boolean[] and no member returns one, so the
      // annotation is legal but nothing can follow it. The refusal lands on the first
      // name, not at the end: there is no point letting the model start.
      {"let a : boolean[] = msg ;\n", "nothing here can produce a boolean[] at all"},
  };
  for (const auto& [text, why] : ill_typed) {
    auto ill = Tokenize(text, by_text);
    GrammarMatcher m(compiled);
    Install(m, env, table, rules);
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
      // The second statement form: the target's own declared type is the requirement.
      "msg = msg.toUpperCase ;\n",
      "msg = count.toString ;\n",
      "count = msg.length ;\n",
      // Parentheses, the only place enter=fresh applies.
      "let a : number = ( count ) ;\n",
      "let a : string = ( msg.toUpperCase ) ;\n",
      "let a : number = ( count ) + count ;\n",
      // The third statement form. It declares a function, which this environment has
      // no type for, so it only has to parse.
      "declare function f ( a : number ) : string ;\n",
      // Chained members: each step retypes the whole expression again.
      "let a : number = msg.split.length ;\n",
      "let a : string = msg.split.join ;\n",
      // No whitespace anywhere, and two statements on one line.
      "let a:number=count;msg=count.toString;",
  };
  for (const auto& text : well_typed) {
    auto ok = Tokenize(text, by_text);
    GrammarMatcher m(compiled);
    Install(m, env, table, rules);
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
