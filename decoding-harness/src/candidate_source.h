// Where candidates come from. A source is given only the committed text; it never
// sees the checker, and it does not decide which candidate is accepted.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace harness {

struct Candidate {
  int32_t token_id;
  std::string bytes;  // what the token writes; ignored for EOS
  double score;       // higher is better
};

class CandidateSource {
 public:
  virtual ~CandidateSource() = default;
  // `bytes` and `token_ids` are what has been committed so far.
  virtual std::vector<Candidate> Propose(const std::string& bytes,
                                         const std::vector<int32_t>& token_ids) = 0;
};

// A test source: for each committed text, a fixed list of candidates (none if the
// text is not in the script). It also writes down every prefix it is given, so tests
// can check that the decoder only ever passes committed text.
class ScriptedSource : public CandidateSource {
 public:
  explicit ScriptedSource(std::map<std::string, std::vector<Candidate>> script);
  std::vector<Candidate> Propose(const std::string& bytes,
                                 const std::vector<int32_t>& token_ids) override;

  std::vector<std::string> seen_bytes;
  std::vector<std::vector<int32_t>> seen_token_ids;

 private:
  std::map<std::string, std::vector<Candidate>> script_;
};

// A test source with no preferences: every call returns the whole list, whatever the
// prefix, each entry with score 0 and token id = its position. EOS is an entry like
// any other; DecodeOptions::eos_token_id says which one it is.
class FixedVocabSource : public CandidateSource {
 public:
  explicit FixedVocabSource(std::vector<std::string> vocab);
  std::vector<Candidate> Propose(const std::string& bytes,
                                 const std::vector<int32_t>& token_ids) override;

 private:
  std::vector<std::string> vocab_;
};

}  // namespace harness
