// ExhaustiveMask against native XGrammar: the same pinned XGrammar, grammar, real
// vocabulary, stop ids and accepted token sequence. At every prefix (from the empty
// one to the last), all 151936 mask entries are compared. EOS is never fed as input,
// but its entries are compared like any other.
#include <xgrammar/xgrammar.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "cases.h"
#include "exhaustive_mask.h"
#include "native_mask.h"
#include "prefix_checker.h"
#include "vocabulary.h"

using namespace harness;
using Mask = std::vector<bool>;

static int failures = 0;
static void check(bool ok, const std::string& what) {
  printf("%s  %s\n", ok ? "pass" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

static const xgrammar::TokenizerInfo& Info() {
  static const xgrammar::TokenizerInfo info =
      LoadQwenTokenizer(std::string(HARNESS_DATA_DIR) + "/qwen2.5-coder-0.5b-instruct");
  return info;
}
static const std::vector<std::string>& Decoded() { return Info().GetDecodedVocab(); }
static const std::vector<int32_t>& Stops() { return Info().GetStopTokenIds(); }
static bool IsStop(int32_t id) { return std::find(Stops().begin(), Stops().end(), id) != Stops().end(); }

// Printable ASCII as is; quote, backslash and everything else escaped.
static std::string Escape(const std::string& s) {
  std::string out;
  for (unsigned char ch : s) {
    char buf[8];
    if (ch == '"' || ch == '\\') {
      out += '\\';
      out += static_cast<char>(ch);
    } else if (ch >= 0x20 && ch < 0x7f) {
      out += static_cast<char>(ch);
    } else {
      snprintf(buf, sizeof buf, "\\x%02X", ch);
      out += buf;
    }
  }
  return out;
}

// Do the bytes end in the middle of a UTF-8 character?
static bool EndsMidCharacter(const std::string& s) {
  int back = 0;
  for (auto it = s.rbegin(); it != s.rend() && back < 4; ++it, ++back) {
    unsigned char ch = *it;
    if ((ch & 0xC0) == 0x80) continue;  // continuation byte
    int need = ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : ch >= 0xC0 ? 2 : 1;
    return back + 1 < need;
  }
  return false;
}

// The id of the token whose decoded bytes are exactly `bytes` (the first, if several).
static int32_t IdOf(const std::string& bytes) {
  auto it = std::find(Decoded().begin(), Decoded().end(), bytes);
  if (it == Decoded().end()) {
    printf("FAIL  \"%s\" is not a single token\n", Escape(bytes).c_str());
    ++failures;
    return -1;
  }
  return static_cast<int32_t>(it - Decoded().begin());
}

struct Coverage {
  int prefixes = 0, complete = 0, complete_and_extendable = 0, mid_character = 0;
};

// Compare the two masks at the current prefix; report every mismatch (up to 20).
static bool CompareAt(const std::string& grammar_name, const std::string& prefix, PrefixChecker& ours,
                      NativeMasker& native, Coverage& cov, Mask* native_out) {
  Mask a = ExhaustiveMask(ours, Decoded(), Stops());
  native.FillBitmask();
  Mask b = BitmaskToMask(native.Bitmask(), Info().GetVocabSize());

  int mismatches = 0;
  bool other_allowed = false;
  for (size_t id = 0; id < a.size(); ++id) {
    if (a[id] && !IsStop(static_cast<int32_t>(id))) other_allowed = true;
    if (a[id] == b[id]) continue;
    if (++mismatches <= 20)
      printf("      MISMATCH grammar %s, prefix \"%s\", id %zu, bytes \"%s\": ours %s, native %s\n",
             grammar_name.c_str(), Escape(prefix).c_str(), id, Escape(Decoded()[id]).c_str(),
             a[id] ? "allow" : "deny", b[id] ? "allow" : "deny");
  }
  if (mismatches > 20) printf("      ... %d mismatches in total at this prefix\n", mismatches);

  ++cov.prefixes;
  if (ours.CanStop()) ++cov.complete;
  if (ours.CanStop() && other_allowed) ++cov.complete_and_extendable;
  if (EndsMidCharacter(prefix)) ++cov.mid_character;
  if (native_out) *native_out = b;
  return mismatches == 0;
}

// Feed `token_ids` to both, comparing masks before the first token and after each one.
static bool CompareSequence(const std::string& grammar_name, const std::string& ebnf,
                            const std::vector<int32_t>& token_ids, Coverage& cov) {
  PrefixChecker ours(ebnf);
  NativeMasker native(Info(), ebnf);
  std::string prefix;
  bool same = CompareAt(grammar_name, prefix, ours, native, cov, nullptr);
  for (int32_t id : token_ids) {
    if (id < 0 || IsStop(id)) return false;  // EOS is never input
    bool ours_ok = ours.TryAppend(Decoded()[id]), native_ok = native.AcceptToken(id);
    if (!ours_ok || !native_ok) {
      printf("      token %d \"%s\" after \"%s\": ours %s, native %s\n", id, Escape(Decoded()[id]).c_str(),
             Escape(prefix).c_str(), ours_ok ? "accepts" : "refuses", native_ok ? "accepts" : "refuses");
      return false;
    }
    prefix += Decoded()[id];
    same = CompareAt(grammar_name, prefix, ours, native, cov, nullptr) && same;
  }
  return same;
}

// A walk of up to `steps` tokens: each step, after the masks are compared, take the
// allowed non-EOS token with the highest hashed score. Stops early if only EOS is left.
static uint64_t Hash(uint64_t seed, uint64_t id, uint64_t step) {
  uint64_t x = seed * 0x9E3779B97F4A7C15ULL ^ id * 0xBF58476D1CE4E5B9ULL ^ step * 0x94D049BB133111EBULL;
  x ^= x >> 31;
  x *= 0xD6E9F8CC1F4E5B37ULL;
  return x ^ (x >> 29);
}

static bool CompareWalk(const std::string& grammar_name, const std::string& ebnf, uint64_t seed, int steps,
                        Coverage& cov, std::string* walked) {
  PrefixChecker ours(ebnf);
  NativeMasker native(Info(), ebnf);
  std::string prefix;
  Mask allowed;
  bool same = CompareAt(grammar_name, prefix, ours, native, cov, &allowed);
  for (int step = 0; step < steps; ++step) {
    int32_t best = -1;
    for (size_t id = 0; id < allowed.size(); ++id) {
      if (allowed[id] && !IsStop(static_cast<int32_t>(id)) &&
          (best < 0 || Hash(seed, id, step) > Hash(seed, best, step)))
        best = static_cast<int32_t>(id);
    }
    if (best < 0) break;
    if (!ours.TryAppend(Decoded()[best]) || !native.AcceptToken(best)) {
      printf("      walk token %d \"%s\" refused after \"%s\"\n", best, Escape(Decoded()[best]).c_str(),
             Escape(prefix).c_str());
      return false;
    }
    prefix += Decoded()[best];
    same = CompareAt(grammar_name, prefix, ours, native, cov, &allowed) && same;
  }
  *walked = prefix;
  return same;
}

int main() {
  const std::vector<GrammarCase> cases = Cases();

  for (const auto& g : cases) {
    Coverage cov;
    std::vector<int32_t> ids;
    for (const auto& t : g.tokens) ids.push_back(IdOf(t));
    check(CompareSequence(g.name, g.ebnf, ids, cov),
          g.name + ": hand-written sequence, every bit at every prefix matches");
    for (uint64_t seed : {1, 2}) {
      std::string walked;
      bool same = CompareWalk(g.name, g.ebnf, seed, 16, cov, &walked);
      check(same, g.name + ": walk seed " + std::to_string(seed) + " (\"" + Escape(walked) + "\") matches");
    }
    printf("      %s: %d prefixes compared (%lld bits); %d complete, %d complete and extendable, "
           "%d mid-character\n",
           g.name.c_str(), cov.prefixes, static_cast<long long>(cov.prefixes) * Info().GetVocabSize(),
           cov.complete, cov.complete_and_extendable, cov.mid_character);
  }
  printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
