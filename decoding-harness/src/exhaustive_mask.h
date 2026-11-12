// A token mask computed by trying every token: one entry per token id, true if the
// token may come next after the checker's current prefix. Uses only the checker
// (CanStop and Probe), never XGrammar's own mask code.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "checker.h"

namespace harness {

// For each id, in this order (the same rules as GreedyDecode):
//   - an id in eos_token_ids: allowed iff checker.CanStop(); its bytes are not used;
//   - empty bytes (e.g. padding ids): denied;
//   - otherwise: allowed iff checker.Probe(decoded_vocab[id]).
// Every token is checked from the same prefix: Probe leaves the checker as it was,
// so the checker is unchanged afterwards.
// Throws std::invalid_argument if an eos id is not in [0, decoded_vocab.size()).
std::vector<bool> ExhaustiveMask(Checker& checker, const std::vector<std::string>& decoded_vocab,
                                 const std::vector<int32_t>& eos_token_ids);

}  // namespace harness
