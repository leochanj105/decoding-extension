"""The vocabulary, and where tokens come from.

A mask here is a set of allowed token ids, or None for "unconstrained". The C++
side will use bit arrays; a set is clearer for a reference implementation and the
only thing the checker asks of a mask is membership.

Two token sources satisfy the same interface, so the driver loop cannot tell them
apart: ReplaySource hands back tokens a model already produced, and a live model
source (later) samples under the mask.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import AbstractSet, Optional, Protocol

Mask = Optional[AbstractSet[int]]


def _byte_decoder() -> dict[str, int]:
    """Inverse of GPT-2's bytes-to-unicode map, used by byte-level BPE vocabularies.

    Byte-level tokenizers store tokens as printable unicode stand-ins for raw
    bytes, so 'Ġ' means a space. This maps each stand-in back to its byte.
    """
    printable = (
        list(range(ord("!"), ord("~") + 1))
        + list(range(0xA1, 0xAD))
        + list(range(0xAE, 0x100))
    )
    stand_ins = printable[:]
    extra = 0
    for byte in range(256):
        if byte not in printable:
            printable.append(byte)
            stand_ins.append(256 + extra)
            extra += 1
    return {chr(c): b for b, c in zip(printable, stand_ins)}


@dataclass
class Vocabulary:
    """Token ids and their decoded text.

    Entries whose bytes are not valid UTF-8, and special tokens using the
    tokenizer's own markup, are absent from `text_of`: they can never form part of
    a program, so the checker has nothing to ask about them.
    """

    text_of: dict[int, str]
    id_of: dict[str, int] = field(init=False)
    size: int = 0

    def __post_init__(self) -> None:
        self.id_of = {}
        for tid, text in self.text_of.items():
            # a text can map to several ids only for added tokens; keep the lowest
            if text not in self.id_of or tid < self.id_of[text]:
                self.id_of[text] = tid

    @classmethod
    def from_tokenizer_json(cls, path: str) -> "Vocabulary":
        raw = json.load(open(path))
        vocab = raw["model"]["vocab"]
        added = {entry["content"] for entry in raw.get("added_tokens", [])}
        decoder = _byte_decoder()
        text_of: dict[int, str] = {}
        highest = -1
        for encoded, tid in vocab.items():
            highest = max(highest, tid)
            if encoded in added:
                continue
            try:
                text_of[tid] = bytes(decoder[ch] for ch in encoded).decode("utf-8")
            except (KeyError, UnicodeDecodeError):
                continue
        for entry in raw.get("added_tokens", []):
            highest = max(highest, entry["id"])
        return cls(text_of=text_of, size=highest + 1)

    def decodable_ids(self) -> list[int]:
        """Ids whose text is known, in ascending order."""
        return sorted(self.text_of)

    def prefix_ids(self, name: str) -> list[int]:
        """Tokens whose text is a prefix of `name`, the whole name included.

        These are the tokens that can begin writing `name`. A token spanning
        `name` and the punctuation around it is deliberately not here -- see
        profiling/measure_straddle.py for why that case needs the parser.
        """
        out = []
        for cut in range(1, len(name) + 1):
            tid = self.id_of.get(name[:cut])
            if tid is not None:
                out.append(tid)
        return out


class TokenSource(Protocol):
    """Where the next token comes from. The driver loop asks; this answers."""

    def next_token(self, mask: Mask, step: int) -> Optional[int]:
        """The next token id, or None when the sequence is over."""
        ...


@dataclass
class Admission:
    """Whether a mask allowed the token that a model actually produced."""

    step: int
    token_id: int
    text: str
    admitted: bool


@dataclass
class ReplaySource:
    """Hands back tokens a model already produced, ignoring the mask.

    The mask is not ignored entirely: every step records whether it would have
    allowed the real token. That is the cheap correctness signal, and on a
    program generated *with* type checking on, every answer must be True.
    """

    token_ids: list[int]
    vocab: Vocabulary
    admissions: list[Admission] = field(default_factory=list)

    @classmethod
    def from_text(cls, text: str, tokenizer, vocab: Vocabulary) -> "ReplaySource":
        return cls(token_ids=list(tokenizer.encode(text).ids), vocab=vocab)

    def next_token(self, mask: Mask, step: int) -> Optional[int]:
        if step >= len(self.token_ids):
            return None
        tid = self.token_ids[step]
        if mask is not None:
            self.admissions.append(
                Admission(
                    step=step,
                    token_id=tid,
                    text=self.vocab.text_of.get(tid, "<undecodable>"),
                    admitted=tid in mask,
                )
            )
        return tid

    def rejected(self) -> list[Admission]:
        """Steps where the mask would have blocked the real token."""
        return [a for a in self.admissions if not a.admitted]
