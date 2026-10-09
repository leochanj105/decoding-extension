// One grammar rule for `let`, covering every type.
//
// The point of this test is what the grammar does NOT contain: no rule per type.
// A single __bind_type reads the annotation the model wrote and makes it the
// requirement for the expression, so the same `let` rule serves number and string.
#include <xgrammar/xgrammar.h>

#include "type_table.h"
#include "type_table_bind.h"
#include <dlpack/dlpack.h>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
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

// ONE let rule. The annotation decides the requirement.
static const char* kGrammar =
    "root      ::= \"let x:\" _type_ann \"=\" _name \";\"\n"
    "_type_ann ::= \"number\" | \"string\"\n"
    "_name     ::= [a-zA-Z_] [a-zA-Z0-9_]*\n";

static const char* kTable =
    "_type_ann finish=require_text\n"
    "_name     finish=produce_text  content=lexicon\n";

static const int T_NUMBER = 0, T_STRING = 1;

/*! \brief Install the inline table on a matcher. */
static void InstallTable(GrammarMatcher& m) {
  TypeTable table;
  std::string error;
  if (!TypeTable::Parse(kTable, &table, &error)) {
    printf("FAIL  the inline table parses: %s\n", error.c_str());
    ++failures;
    return;
  }
  TypeOracle oracle;
  oracle.resolve = [](std::string_view text) -> int32_t {
    if (text == "number") return T_NUMBER;
    if (text == "string") return T_STRING;
    return -1;
  };
  oracle.accepts = [](int32_t need, int32_t have) { return need == have; };
  std::vector<RuleTypeTransition> transitions;
  if (!BuildTransitions(table, m, oracle, &transitions, &error)) {
    printf("FAIL  the table matches the grammar: %s\n", error.c_str());
    ++failures;
    return;
  }
  m.SetRuleTypeTransitions(std::move(transitions));
}

int main(int argc, char** argv) {
  auto vocab = LoadVocab(argv[1]);
  int V = static_cast<int>(vocab.size());
  TokenizerInfo ti(vocab, VocabType::BYTE_LEVEL, V, std::vector<int32_t>{151643});
  GrammarCompiler compiler(ti, 8, false);
  auto compiled = compiler.CompileGrammar(kGrammar, "root");

  // the environment: count is a number, msg is a string
  const std::map<std::string, int32_t> declared = {{"count", T_NUMBER}, {"msg", T_STRING}};

  auto offered = [&](const std::string& prefix) {
    GrammarMatcher m(compiled);
    m.SetLexiconNames(T_NUMBER, {"count"});
    m.SetLexiconNames(T_STRING, {"msg"});
    // number and string are mutually reachable in TypeScript, but for this test keep
    // each tag to its own names so the binding is what the result depends on
    m.SetLexiconReachableTags(T_NUMBER, {T_NUMBER});
    m.SetLexiconReachableTags(T_STRING, {T_STRING});
    InstallTable(m);
    m.SetTypeResolver([&](int32_t, std::string_view matched, int32_t, int32_t) -> int32_t {
      if (matched == "number") return T_NUMBER;
      if (matched == "string") return T_STRING;
      auto it = declared.find(std::string(matched));
      return it == declared.end() ? -1 : it->second;
    });
    if (!m.AcceptString(prefix)) {
      printf("     could not accept %s\n", prefix.c_str());
      return std::set<std::string>{};
    }
    Bitmask bm(V);
    m.FillNextTokenBitmask(&bm.t, 0);
    std::set<std::string> out;
    const auto& decoded = ti.GetDecodedVocab();
    for (int i = 0; i < V; ++i) if (bm.allows(i)) out.insert(decoded[i]);
    return out;
  };

  auto at_number = offered("let x:number=");
  auto at_string = offered("let x:string=");

  check(at_number.count("c") == 1, "after ':number=' the name 'count' is offered");
  check(at_number.count("m") == 0, "after ':number=' the string name 'msg' is not");
  check(at_string.count("m") == 1, "after ':string=' the name 'msg' is offered");
  check(at_string.count("c") == 0, "after ':string=' the number name 'count' is not offered");
  printf("     number annotation: %zu tokens, string annotation: %zu tokens\n",
         at_number.size(), at_string.size());

  check(at_number != at_string,
        "the SAME grammar rule gives different masks, decided by the annotation");

  // With the table installed but no resolver, the annotation cannot be turned into
  // a type, so the position carries no requirement and no name group applies.
  {
    GrammarMatcher m(compiled);
    InstallTable(m);
    m.SetLexiconNames(T_NUMBER, {"count"});
    m.AcceptString("let x:number=");
    Bitmask bm(V);
    m.FillNextTokenBitmask(&bm.t, 0);
    int n = 0;
    for (int i = 0; i < V; ++i) if (bm.allows(i)) ++n;
    check(n == 0, "without a resolver nothing is bound, so nothing is offered");
  }

  // With no table at all, the matcher is plain XGrammar: the grammar says a name is
  // any run of letters and nothing contradicts it. This is the property that keeps
  // every other grammar unaffected by any of this.
  {
    GrammarMatcher m(compiled);
    m.AcceptString("let x:number=");
    Bitmask bm(V);
    m.FillNextTokenBitmask(&bm.t, 0);
    int n = 0;
    for (int i = 0; i < V; ++i) if (bm.allows(i)) ++n;
    check(n > 1000, "with no table the grammar alone decides, offering every name: " +
                        std::to_string(n));
  }

  printf("\n%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
