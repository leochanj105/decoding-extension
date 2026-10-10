// May the expression end here?
//
// This is the check the whole requirement machinery exists for. The environment has
// count : number and msg : string, and the position needs a string.
//
//   let x:string=msg|     may end    -- msg IS a string
//   let x:string=count|   may NOT    -- count is a number; it can still BECOME a
//                                       string via .toString(), so the name was
//                                       admitted, but ending here is wrong
//
// So ';' must be in the mask after msg and absent after count.
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

static const int T_NUMBER = 0, T_STRING = 1;

// __check_expr may only end when what it produced satisfies the requirement.
static const char* kGrammar =
    "root      ::= \"let x:\" _type_ann \"=\" _expr \";\"\n"
    "_type_ann ::= \"number\" | \"string\"\n"
    "_expr     ::= _name\n"
    "_name     ::= [a-zA-Z_] [a-zA-Z0-9_]*\n";

// The same table format as maskgen/fragment.types, inline so the grammar and what
// its rules mean to the types sit side by side in this one test.
static const char* kTable =
    "_type_ann finish=require_text\n"
    "_expr     enter_have=none  finish=check\n"
    "_name     finish=produce_text  content=lexicon\n";

int main(int, char** argv) {
  auto vocab = LoadVocab(argv[1]);
  int V = static_cast<int>(vocab.size());
  TokenizerInfo ti(vocab, VocabType::BYTE_LEVEL, V, std::vector<int32_t>{151643});
  GrammarCompiler compiler(ti, 8, false);
  auto compiled = compiler.CompileGrammar(kGrammar, "root");

  const std::map<std::string, int32_t> declared = {{"count", T_NUMBER}, {"msg", T_STRING}};

  auto configure = [&](GrammarMatcher& m) {
    m.SetLexiconNames(T_NUMBER, {"count"});
    m.SetLexiconNames(T_STRING, {"msg"});
    // number and string are mutually reachable, so BOTH names may be written at a
    // string position -- which is exactly why the stop check has work to do
    m.SetLexiconReachableTags(T_NUMBER, {T_NUMBER, T_STRING});
    m.SetLexiconReachableTags(T_STRING, {T_NUMBER, T_STRING});
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
    m.SetTypeResolver([&](int32_t, std::string_view matched, int32_t, int32_t) -> int32_t {
      if (matched == "number") return T_NUMBER;
      if (matched == "string") return T_STRING;
      auto it = declared.find(std::string(matched));
      return it == declared.end() ? -1 : it->second;
    });
    // assignability: ending requires the exact type, not merely a reachable one
    m.SetTypeAcceptor([](int32_t need, int32_t have) { return need == have; });
  };

  auto offered = [&](const std::string& prefix) {
    GrammarMatcher m(compiled);
    configure(m);
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

  // both names are writable at a string position, since each type reaches the other
  auto start = offered("let x:string=");
  check(start.count("m") == 1, "at a string position 'msg' may be written");
  check(start.count("c") == 1, "at a string position 'count' may ALSO be written");

  // but only one of them may end the expression
  auto after_msg = offered("let x:string=msg");
  auto after_count = offered("let x:string=count");
  check(after_msg.count(";") == 1, "after 'msg' the expression MAY end: ';' is offered");
  check(after_count.count(";") == 0,
        "after 'count' the expression may NOT end: ';' is refused");
  printf("     after msg: %zu tokens, after count: %zu tokens\n",
         after_msg.size(), after_count.size());

  // Worth stating: refusing ';' here leaves NOTHING offered, because this grammar has
  // no way to continue -- __check_expr is just a name. The refusal is correct, but in
  // this grammar it is a dead end, and a model driven by the mask would be stuck.
  //
  // The real grammar must offer the continuation that rescues it: `.` so that
  // count.toString() can be written. Until member access is wired up, a position
  // where the name's type only *reaches* the requirement has no legal future, so the
  // two must land together.
  check(after_count.empty(),
        "after 'count' nothing at all is offered -- correct here, but a dead end "
        "until member access gives it a continuation");

  // and the whole statement follows: one parses, the other does not
  {
    GrammarMatcher ok(compiled, std::nullopt, true);
    configure(ok);
    check(ok.AcceptString("let x:string=msg;") && ok.IsTerminated(),
          "let x:string=msg; parses");
    GrammarMatcher bad(compiled, std::nullopt, true);
    configure(bad);
    check(!(bad.AcceptString("let x:string=count;") && bad.IsTerminated()),
          "let x:string=count; does NOT parse");
  }

  // the number position is the mirror image
  {
    auto after_count_num = offered("let x:number=count");
    auto after_msg_num = offered("let x:number=msg");
    check(after_count_num.count(";") == 1, "at a number position 'count;' ends fine");
    check(after_msg_num.count(";") == 0, "at a number position 'msg;' is refused");
  }

  printf("\n%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
