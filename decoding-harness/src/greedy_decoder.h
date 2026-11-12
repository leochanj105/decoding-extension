// Greedy decoding: at each step, try the source's candidates best first and commit
// the first one the checker accepts.
//
//   order    higher score first; equal scores, lower token id first. Token ids are
//            unique within a list, so this is a total order.
//   token    accepted if checker.TryAppend(bytes) succeeds. A non-EOS candidate with
//            empty bytes is refused: it would commit nothing.
//   EOS      a candidate whose token_id is in eos_token_ids. Accepted only if
//            checker.CanStop(); it writes nothing and is not recorded. Otherwise it is
//            refused like any other candidate.
//   step     one Propose call. max_steps bounds the number of calls.
//
//   kStopped     EOS was accepted.
//   kExhausted   at some step no candidate was accepted (an empty list included).
//   kStepLimit   max_steps calls were made without EOS, even if the program is
//                complete: only EOS ends a run successfully.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "candidate_source.h"
#include "checker.h"

namespace harness {

struct DecodeOptions {
  std::vector<int32_t> eos_token_ids;  // e.g. TokenizerInfo::GetStopTokenIds()
  int max_steps;
};

enum class Outcome { kStopped, kExhausted, kStepLimit };

struct DecodeResult {
  Outcome outcome;
  std::string bytes;               // committed text, EOS excluded
  std::vector<int32_t> token_ids;  // committed tokens, EOS excluded
  int steps;                       // Propose calls made
};

// `checker` must be fresh (nothing committed) and used by this run only; the run
// leaves it holding the committed text. Throws std::invalid_argument if max_steps < 0,
// if the checker is not fresh, or if a candidate list has a duplicate token id or a
// NaN score -- a run that throws has no result, and its checker is not reusable.
DecodeResult GreedyDecode(Checker& checker, CandidateSource& source, const DecodeOptions& options);

}  // namespace harness
