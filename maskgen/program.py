"""Statement structure: what is in scope, and what type is required where.

Answers the question a mask needs and PLDI cannot be asked: *at this point in the
program, which type must the expression here produce?* PLDI never stores that -- it
passes the goal down as an argument during its own recursive check -- so it has to
be computed here.

This stage handles the three statement forms and the symbol table. The expression
body is still opaque: its extent is found by tracking bracket depth to the closing
semicolon, but the type it has committed to so far is not tracked yet.

    let x : number = <expr> ;                     introduces x : number
    x = <expr> ;                                  goal type is x's declared type
    declare function f(a: number): string ;       introduces f : (number) => string

A declaration takes effect only after its semicolon, so `let s : string = s;` is
illegal -- s is not in scope inside its own initializer, which matches PLDI.
"""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass, field

_REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                     "type-constrained-code-generation")
if _REPO not in sys.path:
    sys.path.insert(0, _REPO)

from typesafe_llm.parser.types_ts import (  # noqa: E402
    ArrayPType,
    BooleanPType,
    FunctionPType,
    NumberPType,
    StringPType,
)

from maskgen.lexer import (  # noqa: E402
    KIND_PUNCT,
    KIND_WORD,
    LexError,
    Lexeme,
    lex,
    split_settled,
)

BASE_TYPES = {
    "number": NumberPType,
    "string": StringPType,
    "boolean": BooleanPType,
}


class ParseError(Exception):
    """The text is not a valid prefix of any program in the fragment."""


# What the parser is waiting for. These are the names a mask switches on.
WANT_STATEMENT = "statement"          # let / declare / an identifier
WANT_NEW_NAME = "new-name"            # the name being declared
WANT_COLON = "colon"
WANT_TYPE = "type"                    # a type annotation
WANT_ARRAY_CLOSE = "array-close"      # the ] of number[]
WANT_ASSIGN = "assign"                # =
WANT_EXPRESSION = "expression"        # an expression of required_type
WANT_SEMICOLON = "semicolon"
WANT_FUNCTION = "function"            # the keyword after declare
WANT_NEW_NAME_FUNCTION = "new-function-name"
WANT_PARAM_OPEN = "param-open"        # (
WANT_PARAM_NAME = "param-name"
WANT_PARAM_CLOSE = "param-close"      # )
WANT_RETURN_COLON = "return-colon"


@dataclass(frozen=True)
class Position:
    """Everything the mask needs about where we are."""

    want: str
    scope: dict = field(default_factory=dict)      # name -> type, committed only
    required_type: object | None = None            # for WANT_EXPRESSION
    pending: Lexeme | None = None                  # the unfinished trailing lexeme
    expression_depth: int = 0                      # bracket nesting inside an expression
    expression_started: bool = False               # any lexeme consumed in this expression

    def names_of_type(self, typ) -> list[str]:
        return sorted(n for n, t in self.scope.items() if t == typ)

    def __repr__(self) -> str:
        bits = [f"want={self.want}"]
        if self.required_type is not None:
            bits.append(f"required={self.required_type}")
        if self.pending is not None:
            bits.append(f"pending={self.pending!r}")
        bits.append(f"scope={sorted(self.scope)}")
        return f"Position({', '.join(bits)})"


def _base(name: str):
    maker = BASE_TYPES.get(name)
    if maker is None:
        raise ParseError(f"{name!r} is not a type in this fragment")
    return maker()


@dataclass
class _State:
    """Mutable while walking; a Position is taken from it at the end."""

    want: str = WANT_STATEMENT
    scope: dict = field(default_factory=dict)
    # the statement being built
    kind: str | None = None                  # 'let' | 'assign' | 'declare'
    new_name: str | None = None
    annotation: object | None = None
    param_type: object | None = None
    required_type: object | None = None
    depth: int = 0
    started: bool = False

    def finish_statement(self) -> None:
        if self.kind == "let":
            self.scope[self.new_name] = self.annotation
        elif self.kind == "declare":
            self.scope[self.new_name] = FunctionPType(
                call_signature=(self.param_type,), return_type=self.annotation
            )
        self.kind = None
        self.new_name = None
        self.annotation = None
        self.param_type = None
        self.required_type = None
        self.depth = 0
        self.started = False
        self.want = WANT_STATEMENT


def analyse(text: str) -> Position:
    """Walk `text` and report where we are. Raises ParseError on an invalid prefix."""
    try:
        lexemes = lex(text)
    except LexError as exc:
        raise ParseError(str(exc)) from exc
    settled, pending = split_settled(lexemes)

    s = _State()
    for lexeme in settled:
        _consume(s, lexeme)
    return Position(
        want=s.want,
        scope=dict(s.scope),
        required_type=s.required_type,
        pending=pending,
        expression_depth=s.depth,
        expression_started=s.started,
    )


def _consume(s: _State, lexeme: Lexeme) -> None:
    text, kind = lexeme.text, lexeme.kind

    if s.want == WANT_EXPRESSION:
        # The expression body is opaque at this stage: follow bracket depth until the
        # semicolon that ends the statement.
        if kind == KIND_PUNCT and text in "([":
            s.depth += 1
        elif kind == KIND_PUNCT and text in ")]":
            if s.depth == 0:
                raise ParseError(f"unmatched {text!r}")
            s.depth -= 1
        elif kind == KIND_PUNCT and text == ";" and s.depth == 0:
            if not s.started:
                raise ParseError("empty expression")
            s.finish_statement()
            return
        s.started = True
        return

    if s.want == WANT_STATEMENT:
        if kind == KIND_WORD and text == "let":
            s.kind, s.want = "let", WANT_NEW_NAME
        elif kind == KIND_WORD and text == "declare":
            s.kind, s.want = "declare", WANT_FUNCTION
        elif lexeme.is_identifier:
            if text not in s.scope:
                raise ParseError(f"{text!r} is not declared")
            s.kind, s.new_name = "assign", text
            s.required_type = s.scope[text]
            s.want = WANT_ASSIGN
        else:
            raise ParseError(f"a statement cannot start with {text!r}")
        return

    if s.want == WANT_NEW_NAME or s.want == WANT_PARAM_NAME:
        if not lexeme.is_identifier:
            raise ParseError(f"{text!r} is not a name")
        if s.want == WANT_NEW_NAME:
            if text in s.scope:
                raise ParseError(f"{text!r} is already declared")
            s.new_name = text
        s.want = WANT_COLON
        return

    if s.want == WANT_COLON or s.want == WANT_RETURN_COLON:
        if text != ":":
            raise ParseError(f"expected ':', got {text!r}")
        s.want = WANT_TYPE
        return

    if s.want == WANT_TYPE:
        if kind != KIND_WORD:
            raise ParseError(f"expected a type, got {text!r}")
        s.annotation = _base(text)
        s.want = WANT_ARRAY_CLOSE
        return

    if s.want == WANT_ARRAY_CLOSE:
        # Either `[` `]` making it an array, or we have moved past the type.
        if text == "[":
            s.want = "array-close-bracket"
            return
        return _after_type(s, lexeme)

    if s.want == "array-close-bracket":
        if text != "]":
            raise ParseError(f"expected ']', got {text!r}")
        s.annotation = ArrayPType(s.annotation)
        s.want = WANT_ARRAY_CLOSE
        return

    if s.want == WANT_ASSIGN:
        if text != "=":
            raise ParseError(f"expected '=', got {text!r}")
        s.want, s.started, s.depth = WANT_EXPRESSION, False, 0
        return

    if s.want == WANT_FUNCTION:
        if text != "function":
            raise ParseError(f"expected 'function', got {text!r}")
        s.want = WANT_NEW_NAME_FUNCTION
        return

    if s.want == WANT_NEW_NAME_FUNCTION:
        if not lexeme.is_identifier:
            raise ParseError(f"{text!r} is not a name")
        if text in s.scope:
            raise ParseError(f"{text!r} is already declared")
        s.new_name = text
        s.want = WANT_PARAM_OPEN
        return

    if s.want == WANT_PARAM_OPEN:
        if text != "(":
            raise ParseError(f"expected '(', got {text!r}")
        s.want = WANT_PARAM_NAME
        return

    if s.want == WANT_PARAM_CLOSE:
        if text != ")":
            raise ParseError(f"expected ')', got {text!r}")
        s.want = WANT_RETURN_COLON
        return

    if s.want == WANT_SEMICOLON:
        if text != ";":
            raise ParseError(f"expected ';', got {text!r}")
        s.finish_statement()
        return

    raise ParseError(f"unexpected {text!r} while expecting {s.want}")


def _after_type(s: _State, lexeme: Lexeme) -> None:
    """A type annotation has just finished; what follows depends on the statement."""
    text = lexeme.text
    if s.kind == "let":
        if text != "=":
            raise ParseError(f"expected '=' after the type, got {text!r}")
        s.required_type = s.annotation
        s.want, s.started, s.depth = WANT_EXPRESSION, False, 0
        return
    if s.kind == "declare":
        if s.param_type is None:
            # that was the parameter's type
            s.param_type = s.annotation
            s.annotation = None
            if text != ")":
                raise ParseError(f"expected ')', got {text!r}")
            s.want = WANT_RETURN_COLON
            return
        # that was the return type
        if text != ";":
            raise ParseError(f"expected ';', got {text!r}")
        s.finish_statement()
        return
    raise ParseError(f"unexpected {text!r} after a type")
