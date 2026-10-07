"""Split the fragment's source into lexemes, including an unfinished trailing one.

A mask is needed in the middle of a token, not only between tokens. After `let ms`
the lexer must report a *complete* `let` and an *incomplete* word `ms`, because what
may follow differs: an incomplete word can still grow, a complete one cannot.

So the last lexeme carries `complete=False` when more characters could extend it.
`=` is the subtle case: on its own it may still become `==`.
"""

from __future__ import annotations

from dataclasses import dataclass

WORD_START = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_")
WORD_REST = WORD_START | set("0123456789")
DIGITS = set("0123456789")
WHITESPACE = set(" \t\n")

# Longest first, so == is preferred over =.
PUNCTUATION = ("==", ":", "=", ";", ".", "(", ")", ",", "[", "]", "+")

RESERVED = {"let", "declare", "function", "true", "false",
            "number", "string", "boolean"}

KIND_WORD = "word"
KIND_NUMBER = "number"
KIND_STRING = "string"
KIND_PUNCT = "punct"


@dataclass(frozen=True)
class Lexeme:
    kind: str
    text: str
    complete: bool
    start: int

    @property
    def is_reserved(self) -> bool:
        return self.kind == KIND_WORD and self.text in RESERVED

    @property
    def is_identifier(self) -> bool:
        return self.kind == KIND_WORD and self.text not in RESERVED

    def __repr__(self) -> str:
        tail = "" if self.complete else "..."
        return f"{self.kind}({self.text!r}{tail})"


class LexError(Exception):
    """The text cannot be lexed at all, whatever follows it."""


def lex(text: str) -> list[Lexeme]:
    """Lexemes of `text`. The final one may be incomplete; the rest are complete.

    Raises LexError only for input no continuation can rescue, such as a stray `@`.
    """
    out: list[Lexeme] = []
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch in WHITESPACE:
            i += 1
            continue
        start = i
        if ch in WORD_START:
            j = i + 1
            while j < n and text[j] in WORD_REST:
                j += 1
            # a word touching the end of the input could still grow
            out.append(Lexeme(KIND_WORD, text[i:j], j < n, start))
            i = j
        elif ch in DIGITS:
            j = i + 1
            while j < n and text[j] in DIGITS:
                j += 1
            out.append(Lexeme(KIND_NUMBER, text[i:j], j < n, start))
            i = j
        elif ch == '"':
            j = i + 1
            while j < n and text[j] != '"':
                if text[j] == "\n":
                    raise LexError(f"newline inside a string literal at {j}")
                j += 1
            if j < n:                       # closing quote present
                out.append(Lexeme(KIND_STRING, text[i:j + 1], True, start))
                i = j + 1
            else:                           # still open, can grow
                out.append(Lexeme(KIND_STRING, text[i:], False, start))
                i = n
        else:
            for symbol in PUNCTUATION:
                if text.startswith(symbol, i):
                    # a lone `=` at the very end might still become `==`
                    growable = symbol == "=" and i + 1 == n
                    out.append(Lexeme(KIND_PUNCT, symbol, not growable, start))
                    i += len(symbol)
                    break
            else:
                raise LexError(f"character {ch!r} at {i} cannot start a lexeme")
    return out


def split_settled(lexemes: list[Lexeme]) -> tuple[list[Lexeme], Lexeme | None]:
    """Separate the lexemes that cannot change from a trailing one that still can."""
    if lexemes and not lexemes[-1].complete:
        return lexemes[:-1], lexemes[-1]
    return lexemes, None
