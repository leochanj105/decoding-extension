"""Does the vocabulary decode, and does replay reproduce a program exactly?"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))

from maskgen.tokens import ReplaySource, Vocabulary  # noqa: E402

TOKENIZER = os.path.join(HERE, "..", "..", "profiling", "tok.json")


def load():
    from tokenizers import Tokenizer

    return Tokenizer.from_file(TOKENIZER), Vocabulary.from_tokenizer_json(TOKENIZER)


def test_vocabulary_decodes():
    _, vocab = load()
    assert vocab.size == 151665, vocab.size
    assert len(vocab.text_of) > 140000, len(vocab.text_of)
    # the byte-level stand-in for a space must decode to a real space
    assert vocab.text_of[vocab.id_of[" the"]] == " the"
    assert "Ġ" not in "".join(vocab.text_of[i] for i in list(vocab.text_of)[:5000])


def test_prefix_ids_are_prefixes():
    _, vocab = load()
    ids = vocab.prefix_ids("forEach")
    assert ids, "no token can begin 'forEach'?"
    for tid in ids:
        assert "forEach".startswith(vocab.text_of[tid])
    # the full name is itself a token in this vocabulary
    assert vocab.id_of["forEach"] in ids


def test_replay_reproduces_the_program():
    tok, vocab = load()
    text = 'let x: number = 1;\nconst s: string = "hi";\n'
    source = ReplaySource.from_text(text, tok, vocab)
    seen, step = [], 0
    while True:
        tid = source.next_token(None, step)
        if tid is None:
            break
        seen.append(tid)
        step += 1
    assert tok.decode(seen) == text


def test_replay_audits_the_mask():
    tok, vocab = load()
    source = ReplaySource.from_text("let x = 1;", tok, vocab)
    first = source.token_ids[0]
    assert source.next_token({first}, 0) == first          # mask allows it
    assert source.next_token(set(), 1) == source.token_ids[1]  # mask blocks it
    assert [a.admitted for a in source.admissions] == [True, False]
    assert len(source.rejected()) == 1


if __name__ == "__main__":
    import traceback

    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_"):
            continue
        try:
            fn()
            print(f"pass  {name}")
        except Exception:
            failures += 1
            print(f"FAIL  {name}")
            traceback.print_exc()
    print(f"\n{failures} failure(s)")
    sys.exit(1 if failures else 0)
