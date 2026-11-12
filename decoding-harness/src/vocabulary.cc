#include "vocabulary.h"

#include <picojson.h>

#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace harness {

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

picojson::object ParseObject(const std::string& text, const std::string& path) {
  picojson::value v;
  std::string err = picojson::parse(v, text);
  if (!err.empty() || !v.is<picojson::object>()) throw std::runtime_error("bad JSON in " + path);
  return v.get<picojson::object>();
}

}  // namespace

std::vector<std::string> ReadEncodedVocab(const std::string& tokenizer_json_path) {
  picojson::object tok = ParseObject(ReadFile(tokenizer_json_path), tokenizer_json_path);

  const picojson::object& model = tok.at("model").get<picojson::object>();
  if (!model.at("type").is<std::string>() || model.at("type").get<std::string>() != "BPE")
    throw std::runtime_error(tokenizer_json_path + ": only a BPE model is supported");

  std::vector<std::pair<int64_t, std::string>> entries;
  for (const auto& [token, id] : model.at("vocab").get<picojson::object>()) {
    entries.push_back({id.get<int64_t>(), token});
  }
  for (const auto& added : tok.at("added_tokens").get<picojson::array>()) {
    const auto& obj = added.get<picojson::object>();
    entries.push_back({obj.at("id").get<int64_t>(), obj.at("content").get<std::string>()});
  }

  // Every id from 0 to the largest must be named exactly once.
  std::vector<std::string> vocab(entries.size());
  std::vector<bool> seen(entries.size(), false);
  for (const auto& [id, token] : entries) {
    if (id < 0 || id >= static_cast<int64_t>(entries.size()) || seen[id])
      throw std::runtime_error("token ids in " + tokenizer_json_path + " are not 0..n-1, each once");
    vocab[id] = token;
    seen[id] = true;
  }
  return vocab;
}

xgrammar::TokenizerInfo LoadQwenTokenizer(const std::string& dir) {
  std::vector<std::string> encoded = ReadEncodedVocab(dir + "/tokenizer.json");

  picojson::object config = ParseObject(ReadFile(dir + "/config.json"), dir + "/config.json");
  const picojson::value& size = config.at("vocab_size");
  if (!size.is<int64_t>() || size.get<int64_t>() < static_cast<int64_t>(encoded.size()) ||
      size.get<int64_t>() > std::numeric_limits<int32_t>::max())
    throw std::runtime_error("config.json vocab_size must be an integer, at least the tokenizer's size");
  int32_t vocab_size = static_cast<int32_t>(size.get<int64_t>());
  encoded.resize(vocab_size, "");  // padding ids: empty, hence special in XGrammar

  // eos_token_id is an integer or a list of integers. Each must be a valid id; the
  // check is done on the int64 value, before narrowing, so e.g. 2^32 + 151645 is
  // rejected instead of silently becoming 151645.
  picojson::object gen =
      ParseObject(ReadFile(dir + "/generation_config.json"), dir + "/generation_config.json");
  const picojson::value& eos = gen.at("eos_token_id");
  picojson::array eos_list = eos.is<picojson::array>() ? eos.get<picojson::array>() : picojson::array{eos};
  if (eos_list.empty()) throw std::runtime_error("generation_config.json eos_token_id is empty");
  std::vector<int32_t> stop_ids;
  for (const auto& id : eos_list) {
    if (!id.is<int64_t>() || id.get<int64_t>() < 0 || id.get<int64_t>() >= vocab_size)
      throw std::runtime_error("generation_config.json eos_token_id must be integers in [0, vocab_size)");
    stop_ids.push_back(static_cast<int32_t>(id.get<int64_t>()));
  }

  picojson::object metadata = ParseObject(
      xgrammar::TokenizerInfo::DetectMetadataFromHF(ReadFile(dir + "/tokenizer.json")), "metadata"
  );
  return xgrammar::TokenizerInfo(
      encoded,
      static_cast<xgrammar::VocabType>(metadata.at("vocab_type").get<int64_t>()),
      vocab_size,
      stop_ids,
      metadata.at("add_prefix_space").get<bool>()
  );
}

}  // namespace harness
