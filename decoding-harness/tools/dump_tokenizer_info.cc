// Print the TokenizerInfo built by LoadQwenTokenizer in a plain text format, for
// comparison with scripts/dump_tokenizer_info.py (XGrammar's Python loader):
//   vocab_size N / vocab_type N / add_prefix_space 0|1
//   stop_token_ids  <ids, in TokenizerInfo's order>
//   special_token_ids <ids, in TokenizerInfo's order>
//   one line per id: "<id> <decoded bytes in lowercase hex>"
// Usage: dump_tokenizer_info <model dir>
#include <cstdio>

#include "vocabulary.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <model dir>\n", argv[0]);
    return 2;
  }
  xgrammar::TokenizerInfo info = harness::LoadQwenTokenizer(argv[1]);
  printf("vocab_size %d\n", info.GetVocabSize());
  printf("vocab_type %d\n", static_cast<int>(info.GetVocabType()));
  printf("add_prefix_space %d\n", info.GetAddPrefixSpace() ? 1 : 0);
  printf("stop_token_ids");
  for (int32_t id : info.GetStopTokenIds()) printf(" %d", id);
  printf("\nspecial_token_ids");
  for (int32_t id : info.GetSpecialTokenIds()) printf(" %d", id);
  printf("\n");
  const auto& decoded = info.GetDecodedVocab();
  for (size_t id = 0; id < decoded.size(); ++id) {
    printf("%zu ", id);
    for (unsigned char b : decoded[id]) printf("%02x", b);
    printf("\n");
  }
  return 0;
}
