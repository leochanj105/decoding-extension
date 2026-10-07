"""Does our mask admit exactly the tokens PLDI admits?

The project's claim is "same answers, far less work". If the answers differ the
speed is irrelevant, so this is the test that can falsify the design. It exists
before the implementation it judges, so a pass means something.

Two checks, because they catch opposite faults
----------------------------------------------
`admits_real_tokens` is cheap and broad: replay a program and ask, at every step,
whether the mask would have allowed the token that actually came next. One query
per step. On a program generated *with* type checking on, every answer must be
True, so this catches over-strictness across thousands of programs.

It cannot catch over-permissiveness at all -- a mask admitting everything passes
perfectly. That is what `compare_at` is for: at sampled positions, both sides
answer over the same candidate list and the disagreements are reported by
direction. It costs about 0.29 ms per candidate per side, so the candidate list
is sampled rather than exhaustive.

The two faults are not equally bad. Admitting a token PLDI rejects breaks the
guarantee the system exists to provide -- generated code can be ill-typed.
Rejecting one PLDI admits only makes legal programs ungeneratable.
"""

from __future__ import annotations

import random
from dataclasses import dataclass, field
from typing import Callable, Iterable, Optional, Protocol, Sequence

from maskgen.tokens import Mask, ReplaySource, Vocabulary


class MaskProducer(Protocol):
    """Anything that can say what may follow, and move forward.

    `allowed_among` is the comparison surface, because it is the only thing the
    oracle can do affordably. Our implementation is expected to answer it by
    computing its mask in bulk and intersecting, so the comparison still
    exercises the fast path rather than a per-candidate loop.
    """

    def advance(self, text: str) -> "MaskProducer": ...

    def allowed_among(self, candidates: Iterable[tuple[int, str]]) -> set[int]: ...


@dataclass(frozen=True)
class Disagreement:
    step: int
    token_id: int
    text: str
    direction: str          # "too_permissive" | "too_strict"


@dataclass
class Mutant:
    """A deliberately wrong producer, so the harness can be shown to fail.

    A test suite that cannot fail is worthless, so `drop` makes the wrapped
    producer reject a token it should admit, `add` makes it admit one it should
    reject, and `admit_all` is the degenerate mask of all ones.

    `admit_all` is the important one. It passes `admits_real_tokens` perfectly --
    a mask that allows everything never blocks a real token -- so it is exactly
    the failure the cheap check cannot see, and only `compare_at` catches it.
    """

    inner: MaskProducer
    drop: frozenset[str] = frozenset()
    add: frozenset[str] = frozenset()
    admit_all: bool = False

    def advance(self, text: str) -> "Mutant":
        return Mutant(self.inner.advance(text), self.drop, self.add, self.admit_all)

    def allowed_among(self, candidates: Iterable[tuple[int, str]]) -> set[int]:
        cands = list(candidates)
        if self.admit_all:
            return {tid for tid, _ in cands}
        got = set(self.inner.allowed_among(cands))
        for tid, text in cands:
            if text in self.drop:
                got.discard(tid)
            if text in self.add:
                got.add(tid)
        return got


def compare_at(
    subject: MaskProducer,
    reference: MaskProducer,
    candidates: Sequence[tuple[int, str]],
    step: int = -1,
) -> list[Disagreement]:
    """Both sides answer over the same candidates; report where they differ."""
    mine = subject.allowed_among(candidates)
    theirs = reference.allowed_among(candidates)
    out = []
    for tid, text in candidates:
        in_mine, in_theirs = tid in mine, tid in theirs
        if in_mine == in_theirs:
            continue
        out.append(
            Disagreement(
                step=step,
                token_id=tid,
                text=text,
                direction="too_permissive" if in_mine else "too_strict",
            )
        )
    return out


def _is_ascii_text(text: str) -> bool:
    return bool(text) and all(32 <= ord(ch) < 127 for ch in text)


def sample_candidates(
    vocab: Vocabulary,
    rng: random.Random,
    count: int,
    must_include: Iterable[int] = (),
) -> list[tuple[int, str]]:
    """Up to `count` candidates to compare on, biased towards informative ones.

    `count` is a hard cap, because each candidate costs a reparse on both sides.

    Every ASCII-printable single-character token is always included: there are
    only 95 and they are every punctuation mark, digit and letter, so they carry
    the most signal per query of anything in the vocabulary. The rest of the
    budget is split across short ASCII tokens, longer ASCII tokens, and a slice of
    non-ASCII ones.

    The non-ASCII slice exists because those tokens are not entirely unreachable
    -- a string literal may contain them -- so excluding them outright would leave
    a blind spot inside strings. They are a small share because they cannot appear
    anywhere else in a program.

    Note what this does *not* do: it is a sample, so a clean report bounds the
    disagreement rate rather than proving there is none.
    """
    buckets: dict[str, list[int]] = {"short": [], "long": [], "nonascii": []}
    always: list[int] = []
    for tid, text in vocab.text_of.items():
        if not _is_ascii_text(text):
            buckets["nonascii"].append(tid)
        elif len(text) == 1:
            always.append(tid)
        elif len(text) <= 4:
            buckets["short"].append(tid)
        else:
            buckets["long"].append(tid)

    chosen = set(always)
    chosen.update(tid for tid in must_include if tid in vocab.text_of)

    remaining = count - len(chosen)
    for name, share in (("short", 0.60), ("long", 0.25), ("nonascii", 0.15)):
        if remaining <= 0:
            break
        want = min(int(remaining * share) if share < 1 else remaining,
                   len(buckets[name]))
        if want > 0:
            chosen.update(rng.sample(buckets[name], want))
    return [(tid, vocab.text_of[tid]) for tid in sorted(chosen)]


@dataclass
class Report:
    program: str
    steps: int = 0
    queries: int = 0
    positions_compared: int = 0
    rejected_real_tokens: list = field(default_factory=list)
    disagreements: list[Disagreement] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.rejected_real_tokens and not self.disagreements

    def summary(self) -> str:
        bad = [d for d in self.disagreements if d.direction == "too_permissive"]
        strict = [d for d in self.disagreements if d.direction == "too_strict"]
        return (
            f"{self.program}: {self.steps} steps, "
            f"{self.positions_compared} positions compared, {self.queries} queries\n"
            f"  real tokens the mask would have blocked : {len(self.rejected_real_tokens)}\n"
            f"  too permissive (admitted, PLDI rejects) : {len(bad)}\n"
            f"  too strict     (rejected, PLDI admits)  : {len(strict)}"
        )


def check_program(
    make_subject: Callable[[], MaskProducer],
    make_reference: Callable[[], MaskProducer],
    text: str,
    tokenizer,
    vocab: Vocabulary,
    *,
    compare_every: int = 10,
    candidates_per_position: int = 400,
    max_steps: Optional[int] = None,
    seed: int = 0,
    name: str = "program",
) -> Report:
    """Replay `text`, auditing the real tokens and comparing at sampled positions.

    Both producers are advanced along the same committed text, so a divergence in
    their state shows up as a disagreement rather than silently drifting.
    """
    rng = random.Random(seed)
    source = ReplaySource.from_text(text, tokenizer, vocab)
    subject, reference = make_subject(), make_reference()
    report = Report(program=name)

    step = 0
    while True:
        if max_steps is not None and step >= max_steps:
            break
        tid = source.token_ids[step] if step < len(source.token_ids) else None
        if tid is None:
            break
        token_text = vocab.text_of.get(tid)
        if token_text is None:            # undecodable: nothing to ask about
            break

        # cheap check: would the mask have allowed the token that really came next
        mask: Mask = set(subject.allowed_among([(tid, token_text)]))
        source.next_token(mask, step)
        report.queries += 1

        # expensive check: full comparison at sampled positions
        if step % compare_every == 0:
            cands = sample_candidates(vocab, rng, candidates_per_position, [tid])
            report.disagreements.extend(compare_at(subject, reference, cands, step))
            report.positions_compared += 1
            report.queries += 2 * len(cands)

        subject = subject.advance(token_text)
        reference = reference.advance(token_text)
        step += 1

    report.steps = step
    report.rejected_real_tokens = source.rejected()
    return report
