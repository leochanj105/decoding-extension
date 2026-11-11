#include "candidate_source.h"

#include <utility>

namespace harness {

ScriptedSource::ScriptedSource(std::map<std::string, std::vector<Candidate>> script)
    : script_(std::move(script)) {}

std::vector<Candidate> ScriptedSource::Propose(const std::string& bytes,
                                               const std::vector<int32_t>& token_ids) {
  seen_bytes.push_back(bytes);
  seen_token_ids.push_back(token_ids);
  auto it = script_.find(bytes);
  return it == script_.end() ? std::vector<Candidate>{} : it->second;
}

FixedVocabSource::FixedVocabSource(std::vector<std::string> vocab) : vocab_(std::move(vocab)) {}

std::vector<Candidate> FixedVocabSource::Propose(const std::string&, const std::vector<int32_t>&) {
  std::vector<Candidate> candidates;
  for (size_t i = 0; i < vocab_.size(); ++i) {
    candidates.push_back({static_cast<int32_t>(i), vocab_[i], 0.0});
  }
  return candidates;
}

}  // namespace harness
