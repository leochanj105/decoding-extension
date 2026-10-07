"""Does the parser report the right scope and the right required type?

These are the two things a mask cannot get from PLDI, so they are the two things
worth pinning hard.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))

from maskgen.program import (  # noqa: E402
    WANT_ASSIGN,
    WANT_EXPRESSION,
    WANT_NEW_NAME,
    WANT_STATEMENT,
    WANT_TYPE,
    ParseError,
    analyse,
)
from typesafe_llm.parser.types_ts import (  # noqa: E402
    ArrayPType,
    BooleanPType,
    FunctionPType,
    NumberPType,
    StringPType,
)


def test_walks_a_declaration():
    steps = [
        ("", WANT_STATEMENT),
        ("let ", WANT_NEW_NAME),
        ("let x : ", WANT_TYPE),
        ("let x : number = ", WANT_EXPRESSION),
        ("let x : number = 1;", WANT_STATEMENT),
    ]
    for text, want in steps:
        assert analyse(text).want == want, (text, analyse(text).want)


def test_declaration_takes_effect_only_after_the_semicolon():
    """`let s : string = s;` must not see s, which is what PLDI does too."""
    inside = analyse("let s : string = ")
    assert "s" not in inside.scope, "s is visible inside its own initializer"
    after = analyse("let s : string = \"hi\";")
    assert after.scope["s"] == StringPType()


def test_required_type_comes_from_the_annotation():
    assert analyse("let x : number = ").required_type == NumberPType()
    assert analyse("let s : string = ").required_type == StringPType()
    assert analyse("let b : boolean = ").required_type == BooleanPType()


def test_array_annotations():
    p = analyse("let xs : number[] = ")
    assert p.required_type == ArrayPType(NumberPType())
    p = analyse('let xs : string[] = ["a"]; let n : number = ')
    assert p.scope["xs"] == ArrayPType(StringPType())


def test_required_type_of_an_assignment_comes_from_the_target():
    p = analyse("let s : string = \"a\"; s = ")
    assert p.want == WANT_EXPRESSION
    assert p.required_type == StringPType()


def test_declared_function_enters_scope_with_a_function_type():
    p = analyse("declare function f(a: number): string; ")
    assert p.scope["f"] == FunctionPType(
        call_signature=(NumberPType(),), return_type=StringPType()
    )
    # the parameter name is not in scope: there is no body
    assert "a" not in p.scope


def test_required_type_for_a_call_target_is_the_function_type():
    p = analyse("declare function f(a: number): string; let s : string = ")
    assert p.required_type == StringPType()
    assert sorted(p.scope) == ["f"]


def test_pending_lexeme_is_reported():
    p = analyse("let co")
    assert p.pending is not None and p.pending.text == "co"
    p = analyse("let co ")
    assert p.pending is None


def test_brackets_inside_an_expression_do_not_end_the_statement():
    p = analyse('let s : string = f(g(";"))')
    assert p.want == WANT_EXPRESSION, p
    p = analyse('let s : string = f(g(";"));')
    assert p.want == WANT_STATEMENT and "s" in p.scope


def test_scope_accumulates():
    p = analyse('let x : number = 1; let s : string = "a"; let b : boolean = true; ')
    assert sorted(p.scope) == ["b", "s", "x"]


def test_errors():
    bad = {
        "q = ": "assigning to an undeclared name",
        "let x : number = 1; let x : ": "declaring x twice",
        "let 9x : ": "a name starting with a digit",
        "= 1;": "a statement starting with =",
        "let x : number = ;": "an empty expression",
        "let x : number = )": "an unmatched bracket",
        "declare fun ": "the wrong keyword after declare",
    }
    for text, why in bad.items():
        try:
            got = analyse(text)
        except ParseError:
            continue
        raise AssertionError(f"{why}: {text!r} should not parse, got {got}")


def test_the_parser_reports_pending_lexemes_rather_than_judging_them():
    """A deliberate split of responsibility, worth stating so it is not a hidden hole.

    `let x : widget` is not a valid prefix -- no type in the fragment starts with
    "widget" -- but analyse() accepts it, because `widget` is the pending lexeme and
    pending lexemes are never consumed. Deciding whether a partial word can still
    grow into something legal is the mask's job: it already has to compute the legal
    continuations, and it reports the prefix dead by returning an empty mask.

    If this ever needs to move into the parser, this test is the place it breaks.
    """
    p = analyse("let x : widget")
    assert p.want == WANT_TYPE
    assert p.pending is not None and p.pending.text == "widget"


def test_valid_prefixes_do_not_raise():
    for text in ["", "l", "let", "let ", "let x", "let x ", "let x :", "let x : n",
                 "let x : number", "let x : number ", "let x : number =",
                 "let x : number = ", "let x : number = 1", "let x : number = 1;",
                 "declare", "declare function f(a", "declare function f(a: number",
                 "declare function f(a: number)", "declare function f(a: number):"]:
        analyse(text)


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
