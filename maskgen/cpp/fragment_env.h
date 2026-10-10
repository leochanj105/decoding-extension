// The fragment's environment: the types, the names, their members, and how all of
// that is installed on a matcher.
//
// Shared by the end-to-end test and the benchmark on purpose. A benchmark that
// measures a copy of the setup measures something nothing has verified, and the two
// drift the first time either is touched.
#ifndef MASKGEN_FRAGMENT_ENV_H_
#define MASKGEN_FRAGMENT_ENV_H_

#include <dlpack/dlpack.h>
#include <xgrammar/xgrammar.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "type_table.h"
#include "type_table_bind.h"

namespace maskgen {

using xgrammar::GrammarMatcher;
using xgrammar::RuleTypeTransition;
using xgrammar::GetBitmaskSize;
using xgrammar::GetBitmaskDLType;

inline std::vector<std::string> LoadVocab(const std::string& p) {
  std::ifstream f(p); std::string line; std::getline(f, line);
  int n = std::stoi(line); std::vector<std::string> v; v.reserve(n);
  for (int i = 0; i < n; ++i) { std::getline(f, line); v.push_back(line); }
  return v;
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
inline const char* const kTypeNames[] = {
    "number", "string", "boolean", "number[]", "string[]", "boolean[]"};
inline int32_t MemberTag(int32_t recv, int32_t need) { return 100 + recv * T_NGROUPS + need; }

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
  oracle.resolve_op = [](std::string_view name) -> int32_t { return name == "plus" ? 0 : -1; };
  // TypeScript's `+`: a number only when both operands are numbers, a string as soon
  // as either is one, and nothing otherwise.
  oracle.combine = [](int32_t op, int32_t a, int32_t b) -> int32_t {
    if (op != 0) return -1;
    if (a < 0) return b;
    if (b < 0) return a;
    if (a == T_STR || b == T_STR) return T_STR;
    if (a == T_NUM && b == T_NUM) return T_NUM;
    return -1;
  };
  return oracle;
}

/*! \brief Install an environment on a matcher, driven by the type table. */
inline void Install(
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
  // May an expression of this type *start* here -- looser than the acceptor, which
  // says whether it may stop.
  m.SetTypeReachable([&env](int32_t from, int32_t to) { return env.reachable(from, to); });
  m.SetTypeCombiner([](int32_t op, int32_t a, int32_t b) -> int32_t {
    if (op != 0) return -1;
    if (a < 0) return b;
    if (b < 0) return a;
    if (a == T_STR || b == T_STR) return T_STR;
    if (a == T_NUM && b == T_NUM) return T_NUM;
    return -1;
  });
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

}  // namespace maskgen

#endif  // MASKGEN_FRAGMENT_ENV_H_
