// Latency of generating the next mask after accepting a token: ExhaustiveMask vs
// native XGrammar's FillNextTokenBitmask, on the sequences of cases.h.
//
// Per grammar: 1 warmup run (not recorded), then kRuns runs. Each run builds both
// sides afresh and replays the sequence; after each accepted token, one ExhaustiveMask
// call and one FillBitmask call are timed, each on its own. Not timed: building the
// checker/matcher, accepting tokens, unpacking the native bitmask, comparing masks.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "cases.h"
#include "exhaustive_mask.h"
#include "native_mask.h"
#include "prefix_checker.h"
#include "vocabulary.h"

using namespace harness;
using Clock = std::chrono::steady_clock;

static const int kRuns = 5;

static double Median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  size_t n = v.size();
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

static double MicrosSince(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

int main() {
  const xgrammar::TokenizerInfo info =
      LoadQwenTokenizer(std::string(HARNESS_DATA_DIR) + "/qwen2.5-coder-0.5b-instruct");
  const std::vector<std::string>& vocab = info.GetDecodedVocab();
  const std::vector<int32_t>& stops = info.GetStopTokenIds();

  printf("%-8s %8s %8s %14s %14s %8s\n", "grammar", "masks", "samples", "ours (us)", "native (us)", "ratio");
  for (const GrammarCase& g : Cases()) {
    std::vector<int32_t> ids;
    for (const std::string& t : g.tokens)
      ids.push_back(static_cast<int32_t>(std::find(vocab.begin(), vocab.end(), t) - vocab.begin()));

    std::vector<double> ours_us, native_us;
    for (int run = 0; run <= kRuns; ++run) {  // run 0 is the warmup
      PrefixChecker ours(g.ebnf);
      NativeMasker native(info, g.ebnf);
      for (int32_t id : ids) {
        if (!ours.TryAppend(vocab[id]) || !native.AcceptToken(id)) throw std::runtime_error("token refused");

        Clock::time_point t0 = Clock::now();
        std::vector<bool> mask = ExhaustiveMask(ours, vocab, stops);
        double ours_t = MicrosSince(t0);

        Clock::time_point t1 = Clock::now();
        native.FillBitmask();
        double native_t = MicrosSince(t1);

        // Outside timing: the two masks must agree (also keeps both results used).
        if (BitmaskToMask(native.Bitmask(), info.GetVocabSize()) != mask)
          throw std::runtime_error("masks differ");
        if (run > 0) {
          ours_us.push_back(ours_t);
          native_us.push_back(native_t);
        }
      }
    }
    double o = Median(ours_us), n = Median(native_us);
    printf("%-8s %8zu %8zu %14.1f %14.1f %7.0fx\n", g.name.c_str(), ids.size(), ours_us.size(), o, n, o / n);
  }
  return 0;
}
