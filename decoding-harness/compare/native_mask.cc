#include "native_mask.h"

namespace harness {

NativeMasker::NativeMasker(const xgrammar::TokenizerInfo& info, const std::string& ebnf)
    : matcher_(xgrammar::GrammarCompiler(info).CompileGrammar(ebnf, "root")),
      bitmask_(xgrammar::GetBitmaskSize(info.GetVocabSize()), 0),
      shape_{static_cast<int64_t>(bitmask_.size())},
      tensor_{bitmask_.data(), DLDevice{kDLCPU, 0}, 1, DLDataType{kDLInt, 32, 1}, shape_, nullptr, 0} {}

std::vector<bool> BitmaskToMask(const std::vector<int32_t>& bitmask, int vocab_size) {
  std::vector<bool> mask(vocab_size);
  for (int id = 0; id < vocab_size; ++id) {
    mask[id] = (static_cast<uint32_t>(bitmask[id / 32]) >> (id % 32)) & 1u;
  }
  return mask;
}

}  // namespace harness
