// Does the dynamic lexicon return exactly the tokens that can continue a name?
#include <xgrammar/xgrammar.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include "dynamic_lexicon.h"

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

int main(int argc, char** argv) {
  auto vocab = LoadVocab(argv[1]);
  int V = static_cast<int>(vocab.size());
  TokenizerInfo ti(vocab, VocabType::BYTE_LEVEL, V, std::vector<int32_t>{151643});

  const int TAG_NUMBER = 0, TAG_STRING = 1;
  DynamicLexicon lex(ti);
  lex.SetNames(TAG_NUMBER, {"count", "total", "idx"});
  lex.SetNames(TAG_STRING, {"msg", "name"});

  check(lex.NumTags() == 2, "two tags registered");
  check(lex.Names(TAG_NUMBER).size() == 3, "three number names");
  check(lex.Names(TAG_NUMBER)[0] == "count", "names are sorted");
  check(lex.Names(99).empty(), "an unknown tag has no names");

  // every token offered at the start of a name must be a prefix of some name
  DynamicBitset mask(V);
  int added = lex.FillContinuations(TAG_NUMBER, "", &mask);
  check(added > 0, "start-of-name mask is not empty");
  const auto& decoded = ti.GetDecodedVocab();
  bool all_prefixes = true;
  int offered = 0;
  for (int id = 0; id < V; ++id) {
    if (!mask[id]) continue;
    ++offered;
    const std::string& text = decoded[id];
    bool any = false;
    for (const auto& n : lex.Names(TAG_NUMBER)) {
      if (n.size() >= text.size() && n.compare(0, text.size(), text) == 0) { any = true; break; }
    }
    if (!any) { all_prefixes = false; printf("     offending token %d %s\n", id, text.c_str()); }
  }
  check(all_prefixes, "every offered token is a prefix of some name");
  printf("     %d tokens offered at the start of a number name\n", offered);

  // the tags must not leak into each other
  DynamicBitset smask(V);
  lex.FillContinuations(TAG_STRING, "", &smask);
  bool leaked = false;
  for (int id = 0; id < V; ++id) {
    if (!smask[id]) continue;
    const std::string& t = decoded[id];
    if (t == "c" || t == "co" || t == "count") leaked = true;
  }
  check(!leaked, "a string tag does not offer tokens from number names");

  // mid-name: after "co" only count's continuation is legal
  DynamicBitset partial(V);
  lex.FillContinuations(TAG_NUMBER, "co", &partial);
  bool has_unt = false, has_dx = false;
  for (int id = 0; id < V; ++id) {
    if (!partial[id]) continue;
    if (decoded[id] == "unt") has_unt = true;
    if (decoded[id] == "dx") has_dx = true;
  }
  check(has_unt, "after 'co', the token 'unt' completes count");
  check(!has_dx, "after 'co', nothing from idx is offered");

  check(lex.HasNameWithPrefix(TAG_NUMBER, "co"), "'co' is a live prefix");
  check(!lex.HasNameWithPrefix(TAG_NUMBER, "zz"), "'zz' is dead");
  check(lex.IsCompleteName(TAG_NUMBER, "count"), "'count' is a whole name");
  check(!lex.IsCompleteName(TAG_NUMBER, "cou"), "'cou' is not a whole name");

  // a declaration must only disturb its own tag
  lex.SetNames(TAG_NUMBER, {"count", "total", "idx", "extra"});
  DynamicBitset again(V);
  lex.FillContinuations(TAG_STRING, "", &again);
  check(again == smask, "changing the number tag leaves the string tag's mask alone");

  // cost, since this runs per token
  int reps = 20000;
  DynamicBitset scratch(V);
  auto t0 = Clock::now();
  for (int i = 0; i < reps; ++i) { scratch.Reset(); lex.FillContinuations(TAG_NUMBER, "", &scratch); }
  auto t1 = Clock::now();
  double start_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / reps;
  t0 = Clock::now();
  for (int i = 0; i < reps; ++i) { scratch.Reset(); lex.FillContinuations(TAG_NUMBER, "co", &scratch); }
  t1 = Clock::now();
  double mid_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / reps;
  printf("\n     start of name : %.2f us (sets %d cached token ids)\n", start_us, (int)lex.Names(TAG_NUMBER).size() * 3);
  printf("     mid name 'co' : %.2f us (computed from the surviving names)\n", mid_us);

  printf("\n%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
