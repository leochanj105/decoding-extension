// Does xgrammar's mask come from the runtime lexicon, and does it respect reachability?
//
// The grammar admits any identifier syntactically, so every difference in the mask
// comes from the lexicon.
//
// The tags are deliberately `number` and `number[]`, which are NOT mutually
// reachable: `items.length` turns a number[] into a number, but nothing turns a
// number into a number[]. So a number-typed name must appear at a number[] position
// and NOT the other way round. Picking `number` and `string` would have tested
// nothing, since each reaches the other and both names are legal at both positions --
// an earlier version of this test used that pair and asserted the opposite.
#include <xgrammar/xgrammar.h>
#include <dlpack/dlpack.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace xgrammar;
using Clock = std::chrono::steady_clock;

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
  int count(int vocab) const {
    int c = 0; for (int i = 0; i < vocab; ++i) if (allows(i)) ++c; return c;
  }
};

// n= takes a number name, s= takes a string name. Both lexicon rules match any
// identifier, so only the lexicon distinguishes them.
// n= requires a number, a= requires a number[]
static const char* kGrammar =
    "root ::= (\"n=\" __lex_0 \";\") | (\"a=\" __lex_1 \";\")\n"
    "__lex_0 ::= [a-zA-Z_] [a-zA-Z0-9_]*\n"
    "__lex_1 ::= [a-zA-Z_] [a-zA-Z0-9_]*\n";

static const int TAG_NUMBER = 0, TAG_NUMBER_ARRAY = 1;

int main(int argc, char** argv) {
  auto vocab = LoadVocab(argv[1]);
  int V = static_cast<int>(vocab.size());
  TokenizerInfo ti(vocab, VocabType::BYTE_LEVEL, V, std::vector<int32_t>{151643});
  GrammarCompiler compiler(ti, 8, false);
  auto compiled = compiler.CompileGrammar(kGrammar, "root");

  auto offered = [&](const std::string& prefix) {
    GrammarMatcher m(compiled);
    m.SetLexiconNames(TAG_NUMBER, {"count", "total"});
    m.SetLexiconNames(TAG_NUMBER_ARRAY, {"items"});
    // from the type table: number[] reaches number, number does not reach number[]
    m.SetLexiconReachableTags(TAG_NUMBER, {TAG_NUMBER, TAG_NUMBER_ARRAY});
    m.SetLexiconReachableTags(TAG_NUMBER_ARRAY, {TAG_NUMBER_ARRAY});
    if (!m.AcceptString(prefix)) { printf("     could not accept %s\n", prefix.c_str()); return std::set<std::string>{}; }
    Bitmask bm(V);
    m.FillNextTokenBitmask(&bm.t, 0);
    std::set<std::string> out;
    const auto& decoded = ti.GetDecodedVocab();
    for (int i = 0; i < V; ++i) if (bm.allows(i)) out.insert(decoded[i]);
    return out;
  };

  auto at_number = offered("n=");
  auto at_array = offered("a=");

  check(!at_number.empty(), "the number position offers something");
  check(at_number.count("c") == 1, "number position offers 'c' for count");
  check(at_number.count("count") == 1, "number position offers the whole name 'count'");
  check(at_number.count("t") == 1, "number position offers 't' for total");
  // the reachability half: items is a number[], and items.length is a number
  check(at_number.count("item") == 1,
        "number position DOES offer 'item': a number[] reaches a number via .length");
  // and the other direction must not hold
  check(at_array.count("item") == 1, "number[] position offers 'item'");
  check(at_array.count("c") == 0,
        "number[] position does NOT offer 'c': a number cannot become a number[]");
  check(at_array.count("t") == 0, "number[] position does NOT offer 't' either");
  printf("     number position: %zu tokens, number[] position: %zu tokens\n",
         at_number.size(), at_array.size());

  auto mid = offered("n=co");
  check(mid.count("unt") == 1, "after 'co' the token 'unt' is offered");
  check(mid.count("otal") == 0, "after 'co' nothing from total is offered");
  printf("     after 'n=co': %zu tokens\n", mid.size());

  // the known gap: a token spanning the end of a name is not offered
  check(at_number.count("count;") == 0,
        "straddling token 'count;' is NOT offered (known gap, needs the Advance hook)");

  // a grammar with no lexicon rule must behave exactly as before
  auto plain = compiler.CompileGrammar("root ::= \"ab\" | \"ac\"\n", "root");
  GrammarMatcher pm(plain);
  Bitmask pb(V);
  pm.FillNextTokenBitmask(&pb.t, 0);
  check(pb.count(V) > 0, "a grammar without lexicon rules still produces a mask");

  // cost per mask fill at a lexicon position
  {
    GrammarMatcher m(compiled);
    m.SetLexiconNames(TAG_NUMBER, {"count", "total"});
    m.SetLexiconNames(TAG_NUMBER_ARRAY, {"items"});
    m.SetLexiconReachableTags(TAG_NUMBER, {TAG_NUMBER, TAG_NUMBER_ARRAY});
    m.SetLexiconReachableTags(TAG_NUMBER_ARRAY, {TAG_NUMBER_ARRAY});
    m.AcceptString("n=");
    Bitmask bm(V);
    int reps = 20000;
    auto t0 = Clock::now();
    for (int i = 0; i < reps; ++i) m.FillNextTokenBitmask(&bm.t, 0);
    auto t1 = Clock::now();
    printf("\n     mask fill at a lexicon position: %.2f us\n",
           std::chrono::duration<double, std::micro>(t1 - t0).count() / reps);
  }
  {
    GrammarMatcher pm2(plain);
    Bitmask pb2(V);
    int reps = 20000;
    auto t0 = Clock::now();
    for (int i = 0; i < reps; ++i) pm2.FillNextTokenBitmask(&pb2.t, 0);
    auto t1 = Clock::now();
    printf("     mask fill, plain grammar for comparison: %.2f us\n",
           std::chrono::duration<double, std::micro>(t1 - t0).count() / reps);
  }

  printf("\n%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
