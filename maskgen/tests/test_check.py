"""Can the harness pass when it should, and fail when it should?

The second half matters more. A comparison that never reports a difference would
pass every implementation, correct or not, so Mutant is used to prove detection
works in both directions.
"""

import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))

from maskgen.check import (  # noqa: E402
    Mutant,
    check_program,
    compare_at,
    sample_candidates,
)
from maskgen.oracle import Oracle  # noqa: E402
from maskgen.tokens import Vocabulary  # noqa: E402

TOKENIZER = os.path.join(HERE, "..", "..", "profiling", "tok.json")
PROGRAM = 'let x: number = 1;\nlet s: string = "hi";\nlet t: string = s;\n'


def load():
    from tokenizers import Tokenizer

    return Tokenizer.from_file(TOKENIZER), Vocabulary.from_tokenizer_json(TOKENIZER)


def test_sample_respects_its_budget_and_covers_punctuation():
    _, vocab = load()
    rng = random.Random(0)
    cands = sample_candidates(vocab, rng, 300, must_include=[vocab.id_of["forEach"]])
    ids = {i for i, _ in cands}
    assert vocab.id_of["forEach"] in ids, "must_include was dropped"
    for ch in ";()=.\"":
        assert vocab.id_of[ch] in ids, f"single character {ch!r} missing"
    # the cap is what keeps a position affordable: 36k short tokens would be 21s
    assert len(cands) <= 300 + 1, len(cands)


def test_sample_is_cheap_enough_to_run():
    _, vocab = load()
    cands = sample_candidates(vocab, random.Random(0), 400)
    # 0.29 ms per query per side, so this bounds one position's cost
    assert len(cands) * 2 * 0.00029 < 1.0, f"{len(cands)} candidates is too many"


def test_oracle_agrees_with_itself():
    """Sanity: the harness must not invent differences where none exist."""
    at = Oracle.at_start().advance("let s: string = ")
    _, vocab = load()
    cands = sample_candidates(vocab, random.Random(1), 120)
    assert compare_at(at, at, cands) == []


def test_mutant_that_drops_a_token_is_caught_as_too_strict():
    at = Oracle.at_start().advance("let s: string = ")
    quote = ('"', True)
    cands = [(1, '"'), (2, "x"), (3, "x;")]
    assert at.allows(quote[0])                       # precondition
    bad = Mutant(at, drop=frozenset({'"'}))
    diffs = compare_at(bad, at, cands)
    assert [d.direction for d in diffs] == ["too_strict"], diffs
    assert diffs[0].text == '"'


def test_mutant_that_adds_a_token_is_caught_as_too_permissive():
    at = Oracle.at_start().advance("let s: string = ")
    cands = [(1, '"'), (2, "x;")]
    assert not at.allows("x;")                       # precondition
    bad = Mutant(at, add=frozenset({"x;"}))
    diffs = compare_at(bad, at, cands)
    assert [d.direction for d in diffs] == ["too_permissive"], diffs


def test_check_program_passes_on_the_oracle_itself():
    tok, vocab = load()
    report = check_program(
        make_subject=Oracle.at_start,
        make_reference=Oracle.at_start,
        text=PROGRAM,
        tokenizer=tok,
        vocab=vocab,
        compare_every=6,
        candidates_per_position=150,
        name="self",
    )
    print("     ", report.summary().replace("\n", "\n      "))
    assert report.steps > 10, report.steps
    assert report.positions_compared >= 2
    assert report.ok, report.summary()


def test_a_mask_of_all_ones_is_caught():
    """The failure the cheap check cannot see, so the comparison must catch it.

    A mask admitting everything never blocks a real token, so it passes
    admits_real_tokens perfectly. Only compare_at can tell it is wrong.
    """
    tok, vocab = load()
    report = check_program(
        make_subject=lambda: Mutant(Oracle.at_start(), admit_all=True),
        make_reference=Oracle.at_start,
        text=PROGRAM,
        tokenizer=tok,
        vocab=vocab,
        compare_every=6,
        candidates_per_position=150,
        name="admit-all",
    )
    assert report.rejected_real_tokens == [], "all-ones should pass the cheap check"
    assert not report.ok, "an all-ones mask passed the comparison"
    assert all(d.direction == "too_permissive" for d in report.disagreements)
    print(f"      caught {len(report.disagreements)} too-permissive disagreements")


def test_real_tokens_are_admitted_on_a_constrained_program():
    """The cheap check: PLDI accepted this program, so PLDI must admit every token."""
    tok, vocab = load()
    report = check_program(
        make_subject=Oracle.at_start,
        make_reference=Oracle.at_start,
        text=PROGRAM,
        tokenizer=tok,
        vocab=vocab,
        compare_every=10**9,          # skip the expensive half
        name="admits",
    )
    assert report.rejected_real_tokens == [], report.rejected_real_tokens


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
