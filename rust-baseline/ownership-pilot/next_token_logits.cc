// One forward pass over a rendered input; prints the top-N raw next-token logits at the
// last position (llama_get_logits_ith: the model's output, before any sampler,
// penalty or temperature), as a Markdown table.
// Usage:
//   next_token_logits MODEL.gguf INPUT.txt N [IDS_OUT.txt] > table.md
//       tokenize INPUT.txt; optionally write the evaluated token ids, one per line
//   next_token_logits MODEL.gguf --ids PREFIX_IDS.txt N FORCED_ID... > table.md
//       no tokenization: evaluate the saved ids as one batch, print their top-N (a
//       check), then evaluate each FORCED_ID as its own decode step and print the
//       top-N after each
#include <llama.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Printable ASCII as is; quote, backslash, backtick, '|' and everything else as \xHH.
static std::string Escape(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    char buf[8];
    if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\' && c != '`' && c != '|') {
      out += static_cast<char>(c);
    } else {
      snprintf(buf, sizeof buf, "\\x%02X", c);
      out += buf;
    }
  }
  return out;
}

static std::string Piece(const llama_vocab* vocab, llama_token t) {
  char buf[256];
  int n = llama_token_to_piece(vocab, t, buf, sizeof buf, 0, /*special=*/true);
  return n < 0 ? "<piece error>" : std::string(buf, n);
}

static void PrintLast(const llama_vocab* vocab, const std::vector<llama_token>& toks, int k) {
  printf("Last %d input tokens:\n\n| position | token id | bytes |\n|---:|---:|---|\n", k);
  for (int i = std::max(0, int(toks.size()) - k); i < int(toks.size()); ++i)
    printf("| %d | %d | `\"%s\"` |\n", i, toks[i], Escape(Piece(vocab, toks[i])).c_str());
}

static void PrintTop(llama_context* ctx, const llama_vocab* vocab, int top_n) {
  const int n_vocab = llama_vocab_n_tokens(vocab);
  const float* logits = llama_get_logits_ith(ctx, -1);
  std::vector<int> ids(n_vocab);
  for (int i = 0; i < n_vocab; ++i) ids[i] = i;
  std::partial_sort(ids.begin(), ids.begin() + top_n, ids.end(),
                    [&](int a, int b) { return logits[a] > logits[b]; });
  double z = 0;  // softmax over all n_vocab logits, temperature 1
  for (int i = 0; i < n_vocab; ++i) z += std::exp(double(logits[i]) - logits[ids[0]]);
  printf("\nTop %d next tokens by raw logit (softmax column: computed here over all %d logits, T = 1, "
         "no filtering):\n\n| rank | token id | bytes | raw logit | softmax |\n|---:|---:|---|---:|---:|\n",
         top_n, n_vocab);
  for (int r = 0; r < top_n; ++r) {
    int id = ids[r];
    printf("| %d | %d | `\"%s\"` | %.4f | %.6f |\n", r + 1, id, Escape(Piece(vocab, id)).c_str(), logits[id],
           std::exp(double(logits[id]) - logits[ids[0]]) / z);
  }
}

int main(int argc, char** argv) {
  const bool ids_mode = argc >= 6 && std::string(argv[2]) == "--ids";
  if (!ids_mode && argc != 4 && argc != 5)
    return fprintf(stderr, "usage: %s MODEL INPUT N [IDS_OUT] | MODEL --ids IDS N FORCED_ID\n", argv[0]), 2;

  llama_backend_init();
  llama_model_params mp = llama_model_default_params();
  mp.n_gpu_layers = 0;
  llama_model* model = llama_model_load_from_file(argv[1], mp);
  if (!model) return fprintf(stderr, "cannot load model\n"), 1;
  const llama_vocab* vocab = llama_model_get_vocab(model);

  std::vector<llama_token> toks;
  std::string text;
  int top_n;
  if (ids_mode) {
    std::ifstream in(argv[3]);
    for (llama_token t; in >> t;) toks.push_back(t);
    top_n = std::stoi(argv[4]);
  } else {
    std::ifstream in(argv[2], std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    top_n = std::stoi(argv[3]);
    // Same tokenization as the server's prompt: add_special, parse_special.
    toks.resize(text.size() + 16);
    int n = llama_tokenize(vocab, text.data(), text.size(), toks.data(), toks.size(), true, true);
    if (n < 0) return fprintf(stderr, "tokenize failed\n"), 1;
    toks.resize(n);
    if (argc == 5) {
      std::ofstream out(argv[4]);
      for (llama_token t : toks) out << t << "\n";
    }
  }

  llama_context_params cp = llama_context_default_params();
  cp.n_ctx = 4096;
  cp.n_batch = 4096;
  cp.n_ubatch = 4096;
  cp.n_threads = cp.n_threads_batch = 8;
  llama_context* ctx = llama_init_from_model(model, cp);
  if (!ctx || llama_decode(ctx, llama_batch_get_one(toks.data(), toks.size())) != 0)
    return fprintf(stderr, "decode failed\n"), 1;

  if (!ids_mode) {
    printf("input: %zu bytes, %zu tokens; logits over %d ids at the last position\n\n", text.size(), toks.size(),
           llama_vocab_n_tokens(vocab));
    std::string end = text.substr(text.size() - std::min<size_t>(80, text.size())), esc;
    for (unsigned char c : end) {  // as Escape, but spaces escaped too
      char buf[8];
      snprintf(buf, sizeof buf, "\\x%02X", c);
      esc += c == ' ' ? std::string(buf) : Escape(std::string(1, static_cast<char>(c)));
    }
    printf("Input ends with (last %zu bytes of the file, all whitespace escaped): `\"%s\"`\n\n", end.size(),
           esc.c_str());
    PrintLast(vocab, toks, 8);
    PrintTop(ctx, vocab, top_n);
  } else {
    printf("## Check: the %zu saved prefix ids, evaluated as one batch\n\n", toks.size());
    PrintLast(vocab, toks, 8);
    PrintTop(ctx, vocab, top_n);

    for (int a = 5; a < argc; ++a) {
      llama_token forced = std::stoi(argv[a]);
      if (llama_decode(ctx, llama_batch_get_one(&forced, 1)) != 0) return fprintf(stderr, "decode failed\n"), 1;
      toks.push_back(forced);
      std::string tail;
      for (size_t i = toks.size() - 9; i < toks.size(); ++i) tail += Piece(vocab, toks[i]);
      printf("\n## After forcing token %d (one more decode step on the same context)\n\n", forced);
      printf("Decoded text of the last 9 tokens: `\"%s\"`\n\n", Escape(tail).c_str());
      PrintLast(vocab, toks, 8);
      PrintTop(ctx, vocab, top_n);
    }
  }
  llama_free(ctx);
  llama_model_free(model);
  return 0;
}
