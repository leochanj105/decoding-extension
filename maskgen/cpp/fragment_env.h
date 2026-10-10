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

enum {
  T_NUM = 0, T_STR = 1, T_BOOL = 2, T_NUMA = 3, T_STRA = 4, T_BOOLA = 5,
  /*!
   * \brief The type of `[]`: an array of nothing, TypeScript's never[].
   *
   * It exists because an empty literal has no element to take a type from, and the
   * answer is not "no type" -- it is "an array whose element type is uninhabited",
   * which is assignable to every array and has all the array members. That is what
   * makes `[].length` a number and `let a : number[] = []` legal, both of which this
   * fragment refused until this type existed.
   *
   * No annotation can name it: the grammar's `type` rule lists only the six, so
   * nothing a program writes ever resolves to it.
   */
  T_EMPTYA = 6,
  T_NTYPES = 7
};
// The requirement at a position that imposes none, such as an operand of ==.
enum { T_ANY = T_NTYPES, T_NGROUPS = T_NTYPES + 1 };
inline const char* const kTypeNames[] = {
    "number", "string", "boolean", "number[]", "string[]", "boolean[]", "never[]"};

/*! \brief Whether a type is one of the three array types a program can name. */
inline bool IsNamedArray(int32_t t) { return t == T_NUMA || t == T_STRA || t == T_BOOLA; }
inline int32_t MemberTag(int32_t recv, int32_t need) { return 100 + recv * T_NGROUPS + need; }

/*!
 * \brief Function types, as tags.
 *
 * A method is a function, and a member access hands you that function rather than
 * what calling it gives: `msg.split` is "takes a string, gives a string[]", and only
 * `msg.split("x")` is a string[]. Modelling it the other way round -- which this
 * fragment did until calls were implemented -- accepts TypeScript that does not type
 * check, and leaves the call machinery doing no work at all.
 *
 * kNothing is the parameter of a function that takes none, so `()` has a parameter
 * type to match against like any other call.
 */
enum { kNothing = T_NGROUPS, kParamKinds = T_NGROUPS + 1 };
inline int32_t FnTag(int32_t param, int32_t result) { return 200 + param * kParamKinds + result; }
inline bool IsCallable(int32_t t) { return t >= 200; }
inline int32_t ParamOf(int32_t fn) { return IsCallable(fn) ? (fn - 200) / kParamKinds : -1; }
inline int32_t ResultOf(int32_t fn) { return IsCallable(fn) ? (fn - 200) % kParamKinds : -1; }

/*! \brief The array type whose elements are `element`, or -1 if there is none. */
inline int32_t ArrayOf(int32_t element) {
  switch (element) {
    case T_NUM: return T_NUMA;
    case T_STR: return T_STRA;
    case T_BOOL: return T_BOOLA;
    default: return -1;      // no arrays of arrays in the six
  }
}


struct Environment {
  std::map<std::string, int32_t> symbols{
      {"count", T_NUM}, {"total", T_NUM}, {"msg", T_STR}};
  // A property holds a plain type; a method holds a function type, which has to be
  // called to get anything else. `length` is a property, `split` is a method.
  std::map<int32_t, std::map<std::string, int32_t>> members{
      {T_NUM,
       {{"toString", FnTag(kNothing, T_STR)},
        {"toFixed", FnTag(T_NUM, T_STR)},
        {"valueOf", FnTag(kNothing, T_NUM)}}},
      {T_STR,
       {{"length", T_NUM},
        {"toUpperCase", FnTag(kNothing, T_STR)},
        {"split", FnTag(T_STR, T_STRA)}}},
      {T_BOOL, {{"toString", FnTag(kNothing, T_STR)}}},
      {T_NUMA, {{"length", T_NUM}, {"join", FnTag(T_STR, T_STR)}}},
      {T_STRA, {{"length", T_NUM}, {"join", FnTag(T_STR, T_STR)}}},
      {T_BOOLA, {{"length", T_NUM}, {"join", FnTag(T_STR, T_STR)}}},
      // An array of nothing still has the members every array has.
      {T_EMPTYA, {{"length", T_NUM}, {"join", FnTag(T_STR, T_STR)}}}};
  // Which source types can reach each target, i.e. the transitive closure of the
  // member edges above. A name is admitted at a position if its type reaches what
  // the position requires, since a member access may still get there.
  std::map<int32_t, std::vector<int32_t>> reaches{
      {T_NUM, {T_NUM, T_STR, T_NUMA, T_STRA, T_BOOLA, T_EMPTYA}},
      {T_STR, {T_NUM, T_STR, T_BOOL, T_NUMA, T_STRA, T_BOOLA, T_EMPTYA}},
      {T_BOOL, {T_BOOL}},
      {T_NUMA, {T_NUMA, T_EMPTYA}},
      {T_STRA, {T_STR, T_STRA, T_EMPTYA}},
      {T_BOOLA, {T_BOOLA, T_EMPTYA}},
      {T_EMPTYA, {T_EMPTYA}}};

  /*!
   * \brief Whether a value of type `produced` may stand where `required` is wanted.
   *
   * Assignability, which is not equality: an array of nothing stands for any array.
   * That is the only subtyping in the fragment, and it is what makes `[]` work.
   */
  static bool Accepts(int32_t required, int32_t produced) {
    if (required == produced) return true;
    return produced == T_EMPTYA && IsNamedArray(required);
  }

  /*! \brief Record a declared variable. Later declarations of a name replace it. */
  void Declare(const std::string& name, int32_t type) { symbols[name] = type; }

  /*! \brief The type of `name` as a member of `receiver`, or -1. */
  int32_t member_type(int32_t receiver, std::string_view name) const {
    auto group = members.find(receiver);
    if (group == members.end()) return -1;
    auto it = group->second.find(std::string(name));
    return it == group->second.end() ? -1 : it->second;
  }

  /*!
   * \brief Whether a value of type `from` can still become one of type `to`.
   *
   * A function reaches whatever calling it reaches, which is what lets `.split` be
   * offered at a string[] position even though `.split` itself is a function.
   */
  bool reachable(int32_t from, int32_t to) const {
    if (from == to) return true;
    if (IsCallable(from)) return reachable(ResultOf(from), to);
    auto it = reaches.find(to);
    if (it == reaches.end()) return false;
    for (int32_t s : it->second) if (s == from) return true;
    return false;
  }
};



/*! \brief The element type of an array type, or -1 if it is not an array. */
inline int32_t ElementOf(int32_t array) {
  switch (array) {
    case T_NUMA: return T_NUM;
    case T_STRA: return T_STR;
    case T_BOOLA: return T_BOOL;
    default: return -1;
  }
}

/*!
 * \brief What a position requires, worked out from what encloses it.
 *
 * The requirement that is neither inherited nor written down anywhere. -1 means
 * unconstrained, which is the safe direction: a mask may offer too much, never too
 * little.
 */
inline int32_t Derive(int32_t question, int32_t enclosing_need, int32_t enclosing_have) {
  switch (question) {
    case 0:  // a call argument: the parameter of the function being applied
      return IsCallable(enclosing_have) ? ParamOf(enclosing_have) : -1;
    case 1:  // an array element: what the wanted array holds
      return ElementOf(enclosing_need);
    default: return -1;
  }
}

/*!
 * \brief What an operator does to two types.
 *
 * `accumulated` is what the enclosing position holds and `operand` is what just
 * finished. -1 on either side means nothing yet; -1 out means the operator does not
 * apply, and the position then holds nothing and cannot be finished.
 */
inline int32_t Combine(int32_t op, int32_t accumulated, int32_t operand) {
  switch (op) {
    case 0:  // `+` : a number only when both are numbers, a string if either is
      if (accumulated < 0) return operand;
      if (operand < 0) return accumulated;
      if (accumulated == T_STR || operand == T_STR) return T_STR;
      if (accumulated == T_NUM && operand == T_NUM) return T_NUM;
      return -1;
    case 1: {  // an array literal's element folded into the literal so far
      const int32_t as_array = ArrayOf(operand);
      if (accumulated < 0) return as_array;
      return accumulated == as_array ? accumulated : -1;
    }
    case 2: {  // calling: `accumulated` is the function, `operand` the argument
      if (!IsCallable(accumulated)) return -1;
      const int32_t wanted = ParamOf(accumulated);
      const int32_t given = operand < 0 ? kNothing : operand;
      // Assignability, not equality: passing an argument IS an assignment, so an
      // array of nothing may be passed where any array is wanted. Using equality
      // here refused `h([])`.
      return Environment::Accepts(wanted, given) ? ResultOf(accumulated) : -1;
    }
    default: return -1;
  }
}

/*! \brief Words the language keeps for itself, so no variable may be called one. */
inline bool IsReserved(std::string_view word) {
  for (const char* kw : {"if", "else", "while", "switch", "for", "function", "return",
                         "let", "const", "var", "declare", "true", "false",
                         "number", "string", "boolean"}) {
    if (word == kw) return true;
  }
  return false;
}

/*! \brief The rules this environment has to recognise by name. */
struct RuleIds {
  int32_t member_name = -1;
  int32_t call_step = -1;
  int32_t member_step = -1;
  int32_t arr_lit = -1;
  int32_t arr_empty = -1;
  int32_t decl_name = -1;
};

/*! \brief What the type table needs from this environment. */
inline TypeOracle OracleFor(const Environment& env) {
  TypeOracle oracle;
  oracle.resolve = [&env](std::string_view text) -> int32_t {
    // A tag the table uses for the type of `[]`. The grammar can never name it, so
    // it needs a name of its own here.
    if (text == "empty_array") return T_EMPTYA;
    for (int32_t t = 0; t < T_NTYPES; ++t) {
      if (text == kTypeNames[t]) return t;
    }
    auto sym = env.symbols.find(std::string(text));
    return sym == env.symbols.end() ? -1 : sym->second;
  };
  oracle.accepts = Environment::Accepts;
  oracle.resolve_op = [](std::string_view name) -> int32_t {
    if (name == "plus") return 0;
    if (name == "array") return 1;
    if (name == "apply") return 2;
    if (name == "parameter") return 0;   // a derive question, not an operator
    if (name == "element") return 1;
    return -1;
  };
  // TypeScript's `+`: a number only when both operands are numbers, a string as soon
  // as either is one, and nothing otherwise.
  oracle.combine = Combine;
  oracle.derive = [](std::string_view question, int32_t need, int32_t have) -> int32_t {
    if (question == "parameter") return Derive(0, need, have);
    if (question == "element") return Derive(1, need, have);
    return -1;
  };
  return oracle;
}

/*!
 * \brief Hand the matcher the names currently in scope, by type.
 *
 * Called again after every declaration. Only the one affected type's list actually
 * changes, but pushing all six costs nothing at these sizes and leaves no room for
 * the lists to drift out of step with the environment.
 */
inline void PushSymbolNames(GrammarMatcher& m, const Environment& env) {
  // Names grouped by the type they were declared with. A function-typed name gets a
  // group of its own, because its type is not one of the six.
  std::map<int32_t, std::vector<std::string>> by_tag;
  for (const auto& [name, type] : env.symbols) {
    by_tag[type].push_back(name);
  }

  std::vector<int32_t> function_tags;
  for (const auto& [tag, names] : by_tag) {
    if (IsCallable(tag)) {
      function_tags.push_back(tag);
    }
  }

  for (int32_t t = 0; t < T_NTYPES; ++t) {
    auto it = by_tag.find(t);
    m.SetLexiconNames(t, it == by_tag.end() ? std::vector<std::string>{} : it->second);
    // Which groups a position requiring t draws from: the base types that reach t,
    // plus any function in scope that reaches t by being called. This is where a
    // `declare function` widens the graph -- the model writes a signature and a type
    // that reached nothing before now reaches something.
    std::vector<int32_t> sources = env.reaches.at(t);
    for (int32_t fn : function_tags) {
      if (env.reachable(fn, t)) {
        sources.push_back(fn);
      }
    }
    m.SetLexiconReachableTags(t, sources);
  }

  // A function-typed name is also legal where that exact function type is wanted,
  // which is what makes `f` writable before the `(` that calls it.
  for (int32_t fn : function_tags) {
    m.SetLexiconNames(fn, by_tag.at(fn));
    m.SetLexiconReachableTags(fn, {fn});
  }

  // The any group holds no names of its own; everything in scope can reach it.
  m.SetLexiconNames(T_ANY, {});
  std::vector<int32_t> everything{T_NUM, T_STR, T_BOOL, T_NUMA, T_STRA, T_BOOLA};
  everything.insert(everything.end(), function_tags.begin(), function_tags.end());
  m.SetLexiconReachableTags(T_ANY, everything);
}

/*! \brief Install an environment on a matcher, driven by the type table. */
inline void Install(
    GrammarMatcher& m, const Environment& env, const TypeTable& table, const RuleIds& rules
) {
  PushSymbolNames(m, env);
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
        if (text == "empty_array") return T_EMPTYA;
        for (int32_t t = 0; t < T_NTYPES; ++t) {
          if (text == kTypeNames[t]) return t;
        }
        auto sym = env.symbols.find(std::string(text));
        return sym == env.symbols.end() ? -1 : sym->second;
      }
  );
  m.SetTypeAcceptor(Environment::Accepts);
  // A declared name must be new: not already in scope, and not a reserved word.
  // Checked when the name ends, so prefixes are untouched.
  m.SetTypeDeriver(Derive);
  m.SetNameFilter([&env](int32_t, std::string_view text) {
    return env.symbols.count(std::string(text)) == 0 && !IsReserved(text);
  });
  // May an expression of this type *start* here -- looser than the acceptor, which
  // says whether it may stop.
  m.SetTypeReachable([&env](int32_t from, int32_t to) { return env.reachable(from, to); });
  m.SetTypeCombiner(Combine);
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
  m.SetStepPredicate([rules, &env](int32_t rule, int32_t need, int32_t have) {
    // A call is only worth starting on something callable, which is what makes
    // `count(...)` refused and `msg.split(...)` allowed.
    if (rule == rules.call_step) return IsCallable(have);
    if (rule == rules.member_step) return have >= 0 && !IsCallable(have);
    // An array literal is worth starting wherever some array could still get to
    // what the position wants -- not only where an array is wanted outright, since
    // `[ 1 , 2 ].length` is a number. Reachability, like every other entry gate.
    if (rule == rules.arr_lit) {
      if (need < 0) return true;
      for (int32_t array : {T_NUMA, T_STRA, T_BOOLA}) {
        if (env.reachable(array, need)) return true;
      }
      return false;
    }
    return true;
  });
}

/*!
 * \brief Puts declared variables into scope as the program is written.
 *
 * Call Poll() after every accepted token. It reads the matcher's captures, and on
 * seeing a finished `let` statement takes that statement's name and type and adds
 * them to the environment, then hands the matcher the new name lists.
 *
 * Why it waits for the statement rather than acting as soon as the name and type are
 * known: until the `;` lands the declaration is not final, and a name in scope too
 * early would make `let x : number = x ;` parse.
 *
 * Why captures rather than a hook on the rules: a rule completes thousands of times
 * while a mask is computed, on parse paths the model never takes. XGrammar records a
 * capture only on a committed token, and rolls captures back with the parser, so a
 * name can neither be registered speculatively nor survive a rewind.
 */
class Declarations {
 public:
  Declarations(Environment* env, GrammarMatcher* matcher) : env_(env), matcher_(matcher) {}

  /*! \brief Returns the names declared by this call, for a test or a log. */
  std::vector<std::pair<std::string, int32_t>> Poll() {
    std::vector<std::pair<std::string, int32_t>> fresh;
    const auto captures = matcher_->GetCaptures();
    // Rescan from the start each time rather than remembering an index: with
    // deduplication an earlier entry grows in place as its rule matches more text,
    // so positions are not stable until the occurrence is finished.
    size_t statements = 0;
    std::string name, type, fn_name, param_type, return_type;
    for (const auto& [which, text] : captures) {
      if (which == "name") {
        name = text;
      } else if (which == "type") {
        type = text;
      } else if (which == "fn_name") {
        fn_name = text;
      } else if (which == "param_type") {
        param_type = text;
      } else if (which == "return_type") {
        return_type = text;
      } else if (which == "statement" || which == "declaration") {
        ++statements;
        if (statements <= handled_) {
          continue;
        }
        if (which == "statement" && !name.empty() && !type.empty()) {
          const int32_t t = TypeNamed(type);
          if (t >= 0) {
            env_->Declare(name, t);
            fresh.emplace_back(name, t);
          }
        } else if (which == "declaration" && !fn_name.empty()) {
          // A signature declares a name whose type is a function. Nothing else in
          // the fragment can produce one, so this is the only way the type graph
          // gains an edge the model wrote.
          const int32_t param = TypeNamed(param_type);
          const int32_t result = TypeNamed(return_type);
          if (param >= 0 && result >= 0) {
            const int32_t fn = FnTag(param, result);
            env_->Declare(fn_name, fn);
            fresh.emplace_back(fn_name, fn);
          }
        }
      }
    }
    handled_ = statements;
    if (!fresh.empty()) {
      PushSymbolNames(*matcher_, *env_);
    }
    return fresh;
  }

 private:
  static int32_t TypeNamed(std::string_view text) {
    for (int32_t t = 0; t < T_NTYPES; ++t) {
      if (text == kTypeNames[t]) return t;
    }
    return -1;
  }

  Environment* env_;
  GrammarMatcher* matcher_;
  /*! \brief How many finished statements have already been acted on. */
  size_t handled_ = 0;
};

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
