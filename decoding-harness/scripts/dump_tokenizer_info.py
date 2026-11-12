"""Print the TokenizerInfo that XGrammar's own Python loader builds for the pinned
Qwen tokenizer, in the format of tools/dump_tokenizer_info.cc.

The vocabulary size and stop ids are given explicitly, as numbers written here, not
read by the harness's loader: 151936 is config.json's vocab_size and [151645, 151643]
is generation_config.json's eos_token_id at the pinned revision.

Usage: python dump_tokenizer_info.py <model dir>
"""

import sys

from transformers import AutoTokenizer

import xgrammar as xg

VOCAB_SIZE = 151936
STOP_TOKEN_IDS = [151645, 151643]


def main() -> None:
    tokenizer = AutoTokenizer.from_pretrained(sys.argv[1])
    info = xg.TokenizerInfo.from_huggingface(
        tokenizer, vocab_size=VOCAB_SIZE, stop_token_ids=STOP_TOKEN_IDS
    )
    out = sys.stdout
    out.write(f"vocab_size {info.vocab_size}\n")
    out.write(f"vocab_type {info.vocab_type.value}\n")
    out.write(f"add_prefix_space {int(info.add_prefix_space)}\n")
    out.write("stop_token_ids" + "".join(f" {i}" for i in info.stop_token_ids) + "\n")
    out.write("special_token_ids" + "".join(f" {i}" for i in info.special_token_ids) + "\n")
    for i, token in enumerate(info.decoded_vocab):
        out.write(f"{i} {token.hex()}\n")
    print(f"tokenizer class: {type(tokenizer).__name__}", file=sys.stderr)


if __name__ == "__main__":
    main()
