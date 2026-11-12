#include "greedy_decoder.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace harness {

DecodeResult GreedyDecode(Checker& checker, CandidateSource& source, const DecodeOptions& options) {
  if (options.max_steps < 0) throw std::invalid_argument("max_steps must be >= 0");
  if (checker.CommittedBytes() != 0) throw std::invalid_argument("the checker is not fresh");

  DecodeResult result{Outcome::kStepLimit, "", {}, 0};
  while (result.steps < options.max_steps) {
    std::vector<Candidate> candidates = source.Propose(result.bytes, result.token_ids);
    ++result.steps;

    std::set<int32_t> ids;
    for (const auto& c : candidates) {
      if (std::isnan(c.score) || !ids.insert(c.token_id).second)
        throw std::invalid_argument("a candidate has a NaN score or a duplicate token id");
    }

    // Best first: higher score, then lower token id. Ids are unique, so no two
    // candidates compare equal and the order is fully determined.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
      return a.score != b.score ? a.score > b.score : a.token_id < b.token_id;
    });

    bool committed = false;
    for (const auto& c : candidates) {
      bool is_eos = std::find(options.eos_token_ids.begin(), options.eos_token_ids.end(),
                              c.token_id) != options.eos_token_ids.end();
      if (is_eos) {
        if (checker.CanStop()) {
          result.outcome = Outcome::kStopped;
          return result;
        }
      } else if (!c.bytes.empty() && checker.TryAppend(c.bytes)) {
        result.bytes += c.bytes;
        result.token_ids.push_back(c.token_id);
        committed = true;
        break;
      }
    }
    if (!committed) {
      result.outcome = Outcome::kExhausted;
      return result;
    }
  }
  return result;  // kStepLimit
}

}  // namespace harness
