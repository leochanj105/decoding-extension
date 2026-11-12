#include "exhaustive_mask.h"

#include <stdexcept>

namespace harness {

std::vector<bool> ExhaustiveMask(Checker& checker, const std::vector<std::string>& decoded_vocab,
                                 const std::vector<int32_t>& eos_token_ids) {
  std::vector<bool> is_eos(decoded_vocab.size(), false);
  for (int32_t id : eos_token_ids) {
    if (id < 0 || static_cast<size_t>(id) >= decoded_vocab.size())
      throw std::invalid_argument("an eos token id is outside the vocabulary");
    is_eos[id] = true;
  }

  std::vector<bool> mask(decoded_vocab.size());
  for (size_t id = 0; id < decoded_vocab.size(); ++id) {
    if (is_eos[id]) {
      mask[id] = checker.CanStop();
    } else if (decoded_vocab[id].empty()) {
      mask[id] = false;
    } else {
      mask[id] = checker.Probe(decoded_vocab[id]);
    }
  }
  return mask;
}

}  // namespace harness
