// Tests for the real vocabulary: Qwen2.5-Coder-0.5B-Instruct, pinned in
// scripts/fetch_tokenizer.sh, loaded into XGrammar's TokenizerInfo.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "candidate_source.h"
#include "greedy_decoder.h"
#include "prefix_checker.h"
#include "vocabulary.h"

using namespace harness;

static int failures = 0;
static void check(bool ok, const char* what) {
  printf("%s  %s\n", ok ? "pass" : "FAIL", what);
  if (!ok) ++failures;
}

static const std::string kDir = std::string(HARNESS_DATA_DIR) + "/qwen2.5-coder-0.5b-instruct";

// Loaded on first use, inside main: XGrammar's own globals (e.g. its recursion limit)
// are not guaranteed to be initialised before this file's globals are.
struct Vocab {
  xgrammar::TokenizerInfo info = LoadQwenTokenizer(kDir);
  std::vector<std::string> encoded = ReadEncodedVocab(kDir + "/tokenizer.json");
};
static const Vocab& V() {
  static const Vocab v;
  return v;
}
static const xgrammar::TokenizerInfo& Info() { return V().info; }
static const std::vector<std::string>& Encoded() { return V().encoded; }
static const std::vector<std::string>& Decoded() { return V().info.GetDecodedVocab(); }

// The id of a token by its string as stored in tokenizer.json (e.g. "Ġ="), or -1.
static int IdOf(const std::string& encoded) {
  auto it = std::find(Encoded().begin(), Encoded().end(), encoded);
  return it == Encoded().end() ? -1 : static_cast<int>(it - Encoded().begin());
}
static std::string BytesOf(const std::string& encoded) {
  int id = IdOf(encoded);
  return id < 0 ? "<missing>" : Decoded()[id];
}

static std::vector<int32_t> Sorted(std::vector<int32_t> v) {
  std::sort(v.begin(), v.end());
  return v;
}

static void TestSize() {
  check(Encoded().size() == 151665, "tokenizer.json names ids 0..151664, each once");
  check(Info().GetVocabSize() == 151936 && Decoded().size() == 151936,
        "vocabulary size is config.json's 151936, decoded vocab included");

  std::vector<int32_t> padding;
  for (int32_t i = 151665; i < 151936; ++i) padding.push_back(i);
  bool padding_empty = std::all_of(padding.begin(), padding.end(), [](int32_t i) { return Decoded()[i].empty(); });
  check(padding_empty && Sorted(Info().GetSpecialTokenIds()) == padding,
        "the 271 padding ids decode to nothing and are exactly XGrammar's special ids");
  check(Info().GetVocabType() == xgrammar::VocabType::BYTE_LEVEL && !Info().GetAddPrefixSpace(),
        "detected as byte-level, no prefix space");
}

static void TestSourceUsesXGrammarBytes() {
  std::vector<Candidate> all = FixedVocabSource(Decoded()).Propose("", {});
  bool same = all.size() == Decoded().size();
  for (size_t i = 0; same && i < all.size(); ++i) {
    same = all[i].token_id == static_cast<int32_t>(i) && all[i].bytes == Decoded()[i];
  }
  check(same, "the source offers every id with exactly XGrammar's decoded bytes");
}

static void TestSpacesAndNewlines() {
  check(BytesOf("Ġ") == " " && BytesOf("Ġ=") == " =" && BytesOf("let") == "let",
        "\"Ġ\" -> \" \", \"Ġ=\" -> \" =\", \"let\" -> \"let\"");
  check(BytesOf("Ċ") == "\n" && BytesOf("ĊĊ") == "\n\n" && BytesOf("ĉ") == "\t",
        "\"Ċ\" -> \"\\n\", \"ĊĊ\" -> \"\\n\\n\", \"ĉ\" -> \"\\t\"");
}

static void TestUtf8Fragments() {
  check(BytesOf("Ã") == "\xC3" && BytesOf("©") == "\xA9",
        "\"Ã\" and \"©\" decode to the single bytes 0xC3 and 0xA9 (halves of \"é\")");

  PrefixChecker c(R"(root ::= "s = \"" [^"]* "\";")");
  bool ok = c.TryAppend("s = \"") && c.TryAppend(BytesOf("Ã"));
  check(ok && !c.TryAppend("\""), "  after 0xC3 alone, the string cannot close: the character is incomplete");
  check(c.TryAppend(BytesOf("©")) && c.TryAppend("\";") && c.CanStop(),
        "  0xA9 completes \"é\", and the string closes");
}

static void TestSpecialTokens() {
  check(Sorted(Info().GetStopTokenIds()) == std::vector<int32_t>{151643, 151645},
        "stop ids are generation_config.json's <|endoftext|> 151643 and <|im_end|> 151645");
  const auto& special = Info().GetSpecialTokenIds();
  check(Decoded()[151644] == "<|im_start|>" &&
            std::find(special.begin(), special.end(), 151644) == special.end(),
        "<|im_start|> decodes to its literal text and is not special to XGrammar");
}

// Load from a copy of the model directory whose generation_config.json says
// {"eos_token_id": <eos>}. Returns the stop ids, or {-1} if the loader throws.
static std::vector<int32_t> StopIdsWith(const std::string& eos) {
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / "harness_eos_test";
  fs::remove_all(dir);
  fs::create_directory(dir);
  fs::create_symlink(fs::absolute(kDir + "/tokenizer.json"), dir / "tokenizer.json");
  fs::create_symlink(fs::absolute(kDir + "/config.json"), dir / "config.json");
  std::ofstream(dir / "generation_config.json") << "{\"eos_token_id\": " << eos << "}";
  std::vector<int32_t> ids;
  try {
    ids = LoadQwenTokenizer(dir.string()).GetStopTokenIds();
  } catch (const std::runtime_error&) {
    ids = {-1};
  }
  fs::remove_all(dir);
  return ids;
}

static void TestEosValidation() {
  check(StopIdsWith("151643") == std::vector<int32_t>{151643} &&
            Sorted(StopIdsWith("[151645, 0]")) == std::vector<int32_t>{0, 151645},
        "eos_token_id may be one id or a list of ids");
  bool all_rejected = true;
  for (const char* bad : {"4295118941", "-1", "151936", "151645.5", "\"151645\"", "[]", "[151645, null]"}) {
    all_rejected = all_rejected && StopIdsWith(bad) == std::vector<int32_t>{-1};
  }
  check(all_rejected,
        "rejected before narrowing: 2^32 + 151645, -1, 151936 (= vocab_size), 151645.5, a string, [], null");
}

static void TestDecodeWithRealVocabulary() {
  PrefixChecker checker(R"(root ::= "ab")");
  FixedVocabSource source(Decoded());
  DecodeResult r = GreedyDecode(checker, source, {Info().GetStopTokenIds(), 10});
  check(r.outcome == Outcome::kStopped && r.bytes == "ab" &&
            r.token_ids == std::vector<int32_t>{IdOf("a"), IdOf("b")} && r.steps == 3,
        "decoding \"ab\" over all 151936 ids: \"a\", \"b\", then a stop token");
}

int main() {
  TestSize();
  TestSourceUsesXGrammarBytes();
  TestSpacesAndNewlines();
  TestUtf8Fragments();
  TestSpecialTokens();
  TestEosValidation();
  TestDecodeWithRealVocabulary();
  printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
